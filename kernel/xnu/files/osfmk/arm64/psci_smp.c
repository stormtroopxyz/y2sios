/*
 * More than one processor on generic (non-Apple) ARMv8 boards.
 *
 * On Apple hardware the other cores are powered up by closed drivers. Here
 * the firmware does it, through the standard power interface of ARM systems
 * (PSCI): CPU_ON starts a core at an address of the caller's choice, CPU_OFF
 * stops the calling core for good. The address we give is the kernel's own
 * reset vector (LowResetVectorBase in start.s), so that a new core takes the
 * path Apple's cores take: it finds its cpu data by its MPIDR, turns the MMU
 * on and comes out in arm_init_cpu(). The calls are made with SMC or, under
 * a hypervisor that implements them itself, with HVC; the boot loader says
 * which (/chosen/psci-method).
 *
 * The platform expert (iokit/Kernel/GenericARMPE.cpp) registers the cores of
 * the device tree and starts them here, one after the other. Signals between
 * processors are interrupts of the GIC (gic_v3_irq.c).
 *
 * A new core begins with its MMU and caches off, and what it reads then it
 * reads from memory. On real hardware that is not the same as what the
 * running processors see: under Qualcomm's hypervisor (the phone this was
 * written for) a line that is dirty in another core's cache is simply not
 * there yet. QEMU has no cache and shows none of this. So before CPU_ON
 * everything that core will read before its MMU is on is written back to
 * memory (psci_smp_start_cpu()), and what it writes in that state is read
 * only after throwing away the reader's cached copy.
 *
 * The other half is the way out. The kernel ends by returning to the boot
 * loader (loader_return.c), and the loader gets back only what it started:
 * one core, the boot processor's. psci_smp_stop_others() makes every other
 * core switch itself off first, wherever it is.
 */
#include <mach/mach_types.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/kalloc.h>
#include <kern/misc_protos.h>
#include <libkern/kernel_mach_header.h>
#include <machine/atomic.h>
#include <arm/caches_internal.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_routines.h>
#include <arm64/proc_reg.h>
#include <arm64/psci_smp.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>
#include <vm/pmap.h>
#include <kern/task.h>
#include <kern/thread.h>

#if defined(GENERIC_PSCI_SMP)

#define PSCI_CPU_OFF            0x84000002u
#define PSCI_CPU_ON             0xC4000003u
#define PSCI_AFFINITY_INFO      0xC4000004u
#define PSCI_AFFINITY_OFF       1

#define PSCI_SMP_START_SECONDS  5       /* how long a started core is waited for */
#define PSCI_SMP_STOP_SECONDS   3       /* ... and the cores that were told to stop */

extern void loader_return(void);
extern void loader_log_putc(char c);
extern unsigned long gVirtBase, gPhysBase;

/* The firmware is called with HVC, not SMC. (start.s reads this too, at its physical address.) */
uint32_t psci_smp_hvc;

/*
 * Where the loader lets a core be started from (/chosen psci-cpu-on-trampoline and -slot): code
 * below 4 GB that loads x0 and its entry from the slot. Samsung's firmware (Exynos 990) starts a
 * core only at an address that fits in 32 bits and leaves x0 as it was, not the context_id; the
 * kernel is far above 4 GB. Cores are started one at a time, so one slot is enough.
 */
static uint64_t psci_smp_trampoline;
static uint64_t resume_data_pa;         /* (cpuidle, below) */
static uint64_t psci_smp_slot_pa;
static volatile uint64_t *psci_smp_slot;

/*
 * Left by a core that took an exception between its first instruction and the kernel's vectors
 * (Lsecondary_early_exception in start.s). That core writes it with its cache off; it has a
 * cache line to itself so that looking at memory does not disturb anything else.
 */
struct psci_smp_start_fault {
	uint64_t valid, mpidr, esr, elr, far, lr, sp, unused;
} psci_smp_start_fault __attribute__((aligned(64)));
_Static_assert(sizeof(struct psci_smp_start_fault) == 64, "one cache line");

static uint32_t psci_smp_cpu_stage[MAX_CPUS];   /* PSCI_SMP_CPU_*, by cpu number */
static uint32_t psci_smp_started;               /* a bit per core that CPU_ON was granted for */

static int      psci_smp_stop_owner = -1;       /* the processor that began the return to the boot loader */
static bool     psci_smp_boot_wanted;           /* it was not the boot processor: that one is to finish */
static bool     psci_smp_boot_busy;             /* ... and has begun to */
static bool     psci_smp_stopping;              /* every other core switches itself off */

static int64_t
psci_call(uint64_t function, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = arg1;
	register uint64_t x2 __asm__("x2") = arg2;
	register uint64_t x3 __asm__("x3") = arg3;

	if (psci_smp_hvc) {
		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	} else {
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
                    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	}
	return (int64_t)x0;
}

static const char *
psci_error(int64_t status)
{
	static const char *const names[] = {
		"success", "not supported", "invalid parameters", "denied", "already on", "on pending",
		"internal failure", "not present", "disabled", "invalid address",
	};

	return (status <= 0 && status > -(int64_t)(sizeof(names) / sizeof(names[0]))) ? names[-status] : "?";
}

void
psci_smp_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-method", &prop, &size) == kSuccess &&
	    size >= 3 && strncmp((const char *)prop, "hvc", 3) == 0) {
		psci_smp_hvc = 1;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "psci-cpu-on-trampoline", &prop, &size) == kSuccess && size == 8) {
		memcpy(&psci_smp_trampoline, prop, 8);
		if (SecureDTGetProperty(chosen, "psci-cpu-on-slot", &prop, &size) == kSuccess && size == 8) {
			memcpy(&psci_smp_slot_pa, prop, 8);
		}
		if (psci_smp_slot_pa == 0 || (psci_smp_slot_pa & 15) != 0) {
			psci_smp_trampoline = 0;
		}
	}
}

/* A processor other than the boot processor says how far it has come. */
void
psci_smp_cpu_reached(uint32_t stage)
{
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], stage, release);
}

/* For the rest of the kernel (bsd/dev/redwood_perf.c): has that core joined? The boot processor has. */
int
psci_smp_cpu_is_running(int cpu)
{
	if (cpu == 0) {
		return 1;
	}
	return cpu > 0 && cpu < MAX_CPUS && os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) >= PSCI_SMP_CPU_RUNNING;
}

/* ... all the way: from GenericARMCPU::initCPU(), where the scheduler takes it. */
void
psci_smp_cpu_running(void)
{
	psci_smp_cpu_reached(PSCI_SMP_CPU_RUNNING);
}

static void
psci_smp_clean_segment(const char *name)
{
	unsigned long size = 0;
	vm_offset_t start = (vm_offset_t)getsegdatafromheader(&_mh_execute_header, name, &size);

	if (start != 0 && size != 0) {
		CleanPoC_DcacheRegion(start, size);
	}
}

/*
 * Start a registered processor: the platform's half of cpu_start(). Waits until the core has
 * joined the scheduler, and says what became of it if it has not: on a phone the log is all
 * there is to go by.
 */
kern_return_t
psci_smp_start_cpu(int cpu)
{
	cpu_data_t *cdp = cpu_datap(cpu);
	uint32_t phys_id = cdp->cpu_phys_id, stage;
	uint64_t entry = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	uint64_t now, deadline;
	int64_t status;

	/*
	 * What that core reads with its MMU off: the code and the page tables of start.s, and what
	 * the reset vector finds its way by (ResetHandlerData, CpuDataEntries, the boot arguments,
	 * cpu_ttep, psci_smp_hvc ...), all of it in the kernel's own segments; and the cpu data of
	 * every processor, which the reset vector goes through looking for its own.
	 * Clean, not flush: the processors that are running keep using all of it.
	 */
	psci_smp_start_fault.valid = 0;
	psci_smp_clean_segment("__TEXT");
	psci_smp_clean_segment("__DATA_CONST");
	psci_smp_clean_segment("__TEXT_EXEC");
	psci_smp_clean_segment("__DATA");
	psci_smp_clean_segment("__BOOTDATA");
	for (int other = 0; other <= ml_get_max_cpu_number(); other++) {
		if (cpu_datap(other) != NULL) {
			CleanPoC_DcacheRegion((vm_offset_t)cpu_datap(other), sizeof(cpu_data_t));
		}
	}
	__builtin_arm_dsb(DSB_SY);

	os_atomic_store(&psci_smp_cpu_stage[cpu], PSCI_SMP_CPU_ASKED, release);
	if (psci_smp_trampoline != 0) {
		if (psci_smp_slot == NULL) {
			psci_smp_slot = (volatile uint64_t *)(ml_io_map(trunc_page(psci_smp_slot_pa), PAGE_SIZE) +
			    (psci_smp_slot_pa & PAGE_MASK));
			printf("psci: cores start at the loader's trampoline 0x%llx (slot 0x%llx), then 0x%llx\n",
			    psci_smp_trampoline, psci_smp_slot_pa, entry);
		}
		/* uncached: the new core reads it with its caches off */
		psci_smp_slot[0] = entry;
		psci_smp_slot[1] = phys_id;
		__builtin_arm_dsb(DSB_SY);
		entry = psci_smp_trampoline;
	}
	/* context_id: the core's phys id, how it finds its cpu data when its MPIDR is numbered otherwise (start.s) */
	status = psci_call(PSCI_CPU_ON, phys_id, entry, phys_id);
	if (status != 0) {
		os_atomic_store(&psci_smp_cpu_stage[cpu], 0, release);
		printf("psci: cpu %d (MPIDR 0x%x) was not started: the firmware answers %lld (%s)\n",
		    cpu, phys_id, status, psci_error(status));
		return KERN_FAILURE;
	}
	os_atomic_or(&psci_smp_started, 1u << cpu, relaxed);

	clock_interval_to_deadline(PSCI_SMP_START_SECONDS, NSEC_PER_SEC, &deadline);
	do {
		stage = os_atomic_load(&psci_smp_cpu_stage[cpu], acquire);
		if (stage >= PSCI_SMP_CPU_RUNNING) {
			return KERN_SUCCESS;
		}
		/* memory, not this processor's copy of it */
		FlushPoC_DcacheRegion((vm_offset_t)&psci_smp_start_fault, sizeof(psci_smp_start_fault));
		if (psci_smp_start_fault.valid != 0) {
			break;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
		now = mach_absolute_time();
	} while (now < deadline);

	if (psci_smp_start_fault.valid != 0) {
		struct psci_smp_start_fault *f = &psci_smp_start_fault;
		uint64_t to_virtual = gVirtBase - gPhysBase;

		printf("psci: cpu %d (MPIDR 0x%llx) took an exception on its way in and has switched itself off: "
		    "ESR 0x%llx ELR 0x%llx FAR 0x%llx LR 0x%llx SP 0x%llx (ELR and LR as kernel addresses: 0x%llx 0x%llx)\n",
		    cpu, f->mpidr, f->esr, f->elr, f->far, f->lr, f->sp, f->elr + to_virtual, f->lr + to_virtual);
		os_atomic_andnot(&psci_smp_started, 1u << cpu, relaxed);        /* nothing of it is left to stop */
	} else {
		printf("psci: cpu %d (MPIDR 0x%x) was started %d seconds ago and has not joined: %s\n",
		    cpu, phys_id, PSCI_SMP_START_SECONDS,
		    stage >= PSCI_SMP_CPU_IN_KERNEL ? "it got as far as arm_init_cpu()" : "no sign of it since the firmware took the request");
	}
	return KERN_FAILURE;
}

/*
 * The calling processor switches itself off. Its interrupted work, if any, is abandoned where
 * it was: only for the end of the system.
 */
void
psci_smp_cpu_off(void)
{
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	/* leave nothing behind that would interrupt a core that is no longer there */
	__builtin_arm_wsr64("CNTV_CTL_EL0", CNTV_CTL_EL0_IMASKED);
	gic_v3_irq_cpu_shutdown();
	os_atomic_store(&psci_smp_cpu_stage[cpu_number()], PSCI_SMP_CPU_OFF, release);
	__builtin_arm_dsb(DSB_SY);

	(void) psci_call(PSCI_CPU_OFF, 0, 0, 0);
	for (;;) {
		__builtin_arm_wfi();    /* the firmware refused: stay out of the way all the same */
	}
}

static void
psci_smp_note(const char *what, int cpu)
{
	static const char prefix[] = "\npsci: cpu ";

	for (const char *c = prefix; *c; c++) {
		loader_log_putc(*c);
	}
	loader_log_putc((char)('0' + cpu));
	for (const char *c = what; *c; c++) {
		loader_log_putc(*c);
	}
}

/* Tell every core but this one to switch itself off (psci_smp_stop_here() is where they hear it). */
static void
psci_smp_stop_rest(void)
{
	int me = cpu_number(), max_cpu = ml_get_max_cpu_number();

	os_atomic_store(&psci_smp_stopping, true, release);
	__builtin_arm_dsb(DSB_SY);
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu != me && cdp != NULL && (cpu == master_cpu || (psci_smp_started & (1u << cpu)))) {
			gic_v3_irq_signal_stop(cdp->cpu_phys_id);
		}
	}
	/* for those that wait with their interrupts off (wait_while_mp_kdp_trap(), after a panic) */
	__builtin_arm_sev();
}

/*
 * The system is going back to the boot loader (loader_return()). Returns on the boot processor
 * only, once the other cores are off -- or have been given up on. On any other processor it
 * does not return: that one hands the job to the boot processor, which the loader's side of
 * the return belongs to, and switches itself off like the rest.
 */
void
psci_smp_stop_others(void)
{
	int me, max_cpu;
	uint64_t now, deadline;

	if (psci_smp_started == 0) {
		return;                 /* there is only this one */
	}
	__builtin_arm_wsr("DAIFSet", DAIFSC_ALL);
	me = cpu_number();
	max_cpu = ml_get_max_cpu_number();
	if (!os_atomic_cmpxchg(&psci_smp_stop_owner, -1, me, acq_rel) && psci_smp_stop_owner != me && me != master_cpu) {
		psci_smp_cpu_off();     /* another processor is at it already */
	}

	if (me != master_cpu) {
		/*
		 * Nobody is stopped before the boot processor has answered: a core that is switched
		 * off while it holds a lock the boot processor is waiting for would take the return
		 * with it.
		 */
		psci_smp_note(" ends the system: the return to the boot loader is the boot processor's to make\n", me);
		os_atomic_store(&psci_smp_boot_wanted, true, release);
		__builtin_arm_dsb(DSB_SY);
		gic_v3_irq_signal_stop(cpu_datap(master_cpu)->cpu_phys_id);
		__builtin_arm_sev();
		now = mach_absolute_time();
		nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
		deadline += now;
		while (!os_atomic_load(&psci_smp_stopping, acquire) && now < deadline) {
			now = mach_absolute_time();
		}
		if (!os_atomic_load(&psci_smp_stopping, acquire)) {
			/*
			 * It does not hear (stuck with its interrupts off). There is no return without
			 * it; a loader that watches from its own core will find the kernel silent and
			 * take the log as it is, and so this goes into the log.
			 */
			psci_smp_note(" cannot return to the boot loader and the boot processor does not answer: all cores stop here\n", me);
			psci_smp_stop_rest();
		}
		psci_smp_cpu_off();
	}

	psci_smp_stop_rest();
	now = mach_absolute_time();
	nanoseconds_to_absolutetime(PSCI_SMP_STOP_SECONDS * NSEC_PER_SEC, &deadline);
	deadline += now;
	for (int cpu = 0; cpu <= max_cpu; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);

		if (cpu == me || cdp == NULL || !(psci_smp_started & (1u << cpu))) {
			continue;
		}
		while (now < deadline) {
			if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) == PSCI_SMP_CPU_OFF) {
				/* it has said it is going; the firmware knows when it has gone (if it can tell at all) */
				int64_t state = psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0);

				if (state == PSCI_AFFINITY_OFF || state < 0) {
					break;
				}
			}
			now = mach_absolute_time();
		}
		if (os_atomic_load(&psci_smp_cpu_stage[cpu], acquire) != PSCI_SMP_CPU_OFF &&
		    psci_call(PSCI_AFFINITY_INFO, cdp->cpu_phys_id, 0, 0) != PSCI_AFFINITY_OFF) {
			psci_smp_note(" did not stop: it is left running\n", cpu);
		}
	}
}

/*
 * Where a processor learns that the system is ending: the interrupt psci_smp_stop_rest() sends,
 * and the loop in which the processors wait while another one panics (they have their
 * interrupts off there). Comes back if there is nothing of the kind going on.
 */
void
psci_smp_stop_here(void)
{
	if (psci_smp_started == 0) {
		return;
	}
	if (cpu_number() != master_cpu) {
		if (os_atomic_load(&psci_smp_stopping, acquire)) {
			psci_smp_cpu_off();
		}
		return;
	}
	if (os_atomic_load(&psci_smp_boot_wanted, acquire) && !psci_smp_boot_busy) {
		psci_smp_boot_busy = true;
		loader_return();        /* stops the rest (psci_smp_stop_others()); back only if no loader listens */
		for (;;) {
			__builtin_arm_wfi();
		}
	}
}


/*
 * ==== Idle power-down (boot-arg cpuidle=1) ====
 *
 * An idle core is powered off through the firmware (PSCI CPU_SUSPEND with the device tree's c2 state,
 * arm,psci-suspend-param 0x10000) instead of waiting in WFI clocked and powered. What that needs, and where:
 *  - a way back in: the firmware restarts the core at an address below 4 GB without its context, so the
 *    loader's kernel_cpu_resume finds it by its MPIDR in a table this file fills (xnuloader/loader.c) and
 *    enters the kernel's reset vector, where XNU's own non-retention path takes it (cpu_reset_handler is
 *    resume_idle_cpu: start.s -> arm_init_idle_cpu -> redwood_cpuidle_resumed -> Idle_load_context);
 *  - a way to be woken: the ARM timer is `use-clocksource-only` on this SoC (its comparator does not wake
 *    a powered-off core), so the next deadline is set on the core's own MCT local timer (Samsung's
 *    exynos_mct.c: L_n at 0x10040300 + 0x100 n, 26 MHz / 2, GIC SPI 0x1d6 + n) for the power-down only;
 *  - what the core loses: the GIC's CPU interface (gic_v3_irq_cpu_resume), the timer's comparator
 *    (timer_resync_deadlines), jit_flush's SCTLR bit (redwood_uci_off).
 * Only when the next deadline is past the state's minimum residency (dtb: 2 ms A55, 1 ms A76 and M5).
 * Armed cpuidle_after= seconds after boot (default 60). Counted in the 10 s dvfs: report.
 */
#define PSCI_CPU_SUSPEND        0xC4000001u
#define PSCI_C2_POWER_DOWN      0x10000u
#define MCT_PHYS                0x10040000ULL
#define MCT_L(n)                (0x300u + 0x100u * (n))
#define MCT_L_TCNTB             0x00u
#define MCT_L_ICNTB             0x08u
#define MCT_L_TCON              0x20u
#define MCT_L_INT_CSTAT         0x30u
#define MCT_L_INT_ENB           0x34u
#define MCT_L_WSTAT             0x40u
#define MCT_L_TCON_INT_START    (1u << 1)
#define MCT_L_TCON_TIMER_START  (1u << 0)
#define MCT_INTID(n)            (32u + 0x1d6u + (n))

uint64_t redwood_cpu_mpidr[8];          /* each core's MPIDR | bit 63, at its first cpu_init (cpu.c) */
static bool redwood_cpuidle_on;
static volatile uint8_t *mct;
static volatile uint64_t *resume_data;
static uint64_t resume_tramp_pa;
static uint64_t residency_abs[8], latency_abs[8];
struct redwood_cpuidle_count {
	uint64_t entered, resumed, returned, refused, skipped, slept_abs;
};
struct redwood_cpuidle_count redwood_cpuidle_counts[8];
static uint64_t suspend_at[8];
uint64_t redwood_cpuidle_resumed_at[8]; /* each core's last return from a power-down (mach time) */
extern void gic_v3_irq_cpu_resume(void);
extern kern_return_t gic_v3_spi_attach_to(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context, int cpu);
extern uint32_t gic_resume_seen[][2];

/*
 * The PMU's part (Samsung: cpuidle-exynos64.c -> exynos_cpu_pm_enter/exit -> cal_cpu_disable/enable, the
 * sequences of cal-if/exynos9830/flexpmu_cal_cpu_exynos9830.h): without it the firmware's CPU_SUSPEND is a WFI
 * with the core left powered. Before: the core's CPU_INFORM says C2 (pmucal_powermode_hint), its wake-up
 * interrupt to the PMU is enabled (PMU_INTR_GEN GRP2_INTR_BID_ENABLE bit n), pending GRP1 bits n and n + 8
 * cleared, and its power-down request interrupt enabled (CLUSTERc_CPUi_INT_EN bit 3). After: the reverse
 * (core*_on). Under one lock, as Samsung's cpupm_lock: the GRP registers are shared.
 */
#define PMU_ALIVE_PHYS          0x15860000ULL
#define PMU_INTR_GEN_PHYS       0x15870000ULL
#define PMU_CPU_INFORM(n)       (0x0860u + 4u * (n))
#define PMU_CPU_INFORM_C2       1u
#define PMU_GRP1_PEND           0x0108u
#define PMU_GRP1_CLEAR          0x010cu
#define PMU_GRP2_ENABLE         0x0200u
#define PMU_GRP2_PEND           0x0208u
#define PMU_GRP2_CLEAR          0x020cu
static volatile uint8_t *pmu_alive, *pmu_intr;
static uint32_t pmu_lock;

/* CLUSTERc_CPUi_INT_EN of core n in the dtb's order (0-3 cluster 0, 4-5 cluster 1, 6-7 cluster 2) */
static uint32_t
pmu_int_en(unsigned n)
{
	return n < 4 ? 0x1044u + 0x80u * n : n < 6 ? 0x1344u + 0x80u * (n - 4) : 0x1544u + 0x80u * (n - 6);
}

static uint32_t
pmu_read(volatile uint8_t *base, uint32_t reg)
{
	return *(volatile uint32_t *)(base + reg);
}

static void
pmu_write(volatile uint8_t *base, uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(base + reg) = value;
}

static void
pmu_core(unsigned n, bool off)
{
	while (!os_atomic_cmpxchg(&pmu_lock, 0, 1, acquire)) {
		__builtin_arm_yield();
	}
	if (off) {
		pmu_write(pmu_alive, PMU_CPU_INFORM(n), PMU_CPU_INFORM_C2);
		pmu_write(pmu_intr, PMU_GRP2_ENABLE, pmu_read(pmu_intr, PMU_GRP2_ENABLE) | 1u << n);
		pmu_write(pmu_intr, PMU_GRP1_CLEAR, pmu_read(pmu_intr, PMU_GRP1_PEND) & (1u << n));
		pmu_write(pmu_intr, PMU_GRP1_CLEAR, pmu_read(pmu_intr, PMU_GRP1_PEND) & (1u << (n + 8)));
		pmu_write(pmu_alive, pmu_int_en(n), pmu_read(pmu_alive, pmu_int_en(n)) | 1u << 3);
	} else {
		pmu_write(pmu_intr, PMU_GRP2_ENABLE, pmu_read(pmu_intr, PMU_GRP2_ENABLE) & ~(1u << n));
		pmu_write(pmu_intr, PMU_GRP2_CLEAR, pmu_read(pmu_intr, PMU_GRP2_PEND) & (1u << n));
		pmu_write(pmu_alive, pmu_int_en(n), pmu_read(pmu_alive, pmu_int_en(n)) & ~(1u << 3));
	}
	__builtin_arm_dsb(DSB_SY);
	os_atomic_store(&pmu_lock, 0, release);
}
extern bool redwood_jit_flush_on;
extern void redwood_uci_off(void *arg);
extern void timer_resync_deadlines(void);

static void
mct_write(unsigned n, uint32_t reg, uint32_t value, uint32_t wstat)
{
	*(volatile uint32_t *)(mct + MCT_L(n) + reg) = value;
	if (wstat != 0) {
		for (unsigned spin = 0; spin < 100000; spin++) {
			if (*(volatile uint32_t *)(mct + MCT_L(n) + MCT_L_WSTAT) & wstat) {
				*(volatile uint32_t *)(mct + MCT_L(n) + MCT_L_WSTAT) = wstat;
				break;
			}
		}
	}
}

/* core n's local timer stopped and its interrupt cleared (exynos4_mct_tick_stop) */
static void
mct_stop(unsigned n)
{
	mct_write(n, MCT_L_INT_CSTAT, 1, 0);
	mct_write(n, MCT_L_TCON, *(volatile uint32_t *)(mct + MCT_L(n) + MCT_L_TCON) &
	    ~(MCT_L_TCON_INT_START | MCT_L_TCON_TIMER_START | (1u << 2)), 1u << 3);
}

/* ... to interrupt after `ticks` of 13 MHz (exynos4_mct_tick_start, one-shot) */
static void
mct_start(unsigned n, uint32_t ticks)
{
	mct_write(n, MCT_L_ICNTB, (1u << 31) | ticks, 1u << 1);
	mct_write(n, MCT_L_INT_ENB, 1, 0);
	mct_write(n, MCT_L_TCON, MCT_L_TCON_INT_START | MCT_L_TCON_TIMER_START, 1u << 3);
}

/* the wake-up itself: nothing to do but quiet it (the core's deadlines are the ARM timer's again) */
static void
mct_interrupt(void *context, uint32_t intid __unused)
{
	mct_stop((unsigned)(uintptr_t)context);
}

static void
redwood_cpuidle_arm(thread_call_param_t p0 __unused, thread_call_param_t p1 __unused)
{
	extern void m2m_klog(char sev, const char *cat, const char *fmt, ...);
	uint64_t us;
	unsigned cores = 0;

	mct = (volatile uint8_t *)ml_io_map(MCT_PHYS, 0x1000);
	pmu_alive = (volatile uint8_t *)ml_io_map(PMU_ALIVE_PHYS, 0x2000);
	pmu_intr = (volatile uint8_t *)ml_io_map(PMU_INTR_GEN_PHYS, 0x1000);
	if (pmu_alive == NULL || pmu_intr == NULL) {
		m2m_klog('E', "DEV", "cpuidle: the PMU could not be mapped: not armed");
		return;
	}
	resume_data = (volatile uint64_t *)(ml_io_map(trunc_page(resume_data_pa), PAGE_SIZE) + (resume_data_pa & PAGE_MASK));
	if (mct == NULL || resume_data == NULL) {
		m2m_klog('E', "DEV", "cpuidle: the MCT or the loader's resume table could not be mapped: not armed");
		return;
	}
	{
		int64_t version = psci_call(0x84000000u, 0, 0, 0);
		int64_t features = psci_call(0x8400000Au, PSCI_CPU_SUSPEND, 0, 0);
		uint32_t g_tcon = *(volatile uint32_t *)(mct + 0x240), cnt0 = *(volatile uint32_t *)(mct + 0x100), cnt1;

		delay(100);
		cnt1 = *(volatile uint32_t *)(mct + 0x100);
		m2m_klog('I', "DEV", "cpuidle: PSCI version %lld.%lld, CPU_SUSPEND features %#llx (bit 1: extended state ids); "
		    "MCT G_TCON %#x, its counter %u -> %u in 100 us", version >> 16, version & 0xffff, features, g_tcon,
		    cnt0, cnt1);
		if (features < 0 || (features & 2)) {
			/* not there, or 0x10000 would mean something else in the extended format */
			m2m_klog('E', "DEV", "cpuidle: CPU_SUSPEND not usable with the dtb's original-format state: not armed");
			return;
		}
	}
	resume_data[0] = ml_static_vtop((vm_offset_t)&LowResetVectorBase);
	for (int cpu = 0; cpu <= ml_get_max_cpu_number() && cpu < 8; cpu++) {
		cpu_data_t *cdp = cpu_datap(cpu);
		uint64_t mpidr = redwood_cpu_mpidr[cpu];
		bool small = cpu < 4;   /* the dtb's order: 0-3 Cortex-A55, 4-5 Cortex-A76, 6-7 M5 */

		if (cdp == NULL || !(mpidr & (1ULL << 63))) {
			continue;
		}
		resume_data[1 + 2 * cpu] = mpidr & 0xffffff;
		resume_data[2 + 2 * cpu] = cdp->cpu_phys_id;
		/* dtb idle-states: min-residency 2000/1000/1000 us; entry + exit 70+160, 150+190, 235+220 us */
		nanoseconds_to_absolutetime((small ? 2000 : 1000) * NSEC_PER_USEC, &residency_abs[cpu]);
		us = cpu < 4 ? 230 : cpu < 6 ? 340 : 455;
		nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &latency_abs[cpu]);
		mct_write(cpu, MCT_L_TCNTB, 1, 1u << 0);         /* TICK_BASE_CNT: 26 MHz / 2 */
		mct_stop(cpu);
		m2m_klog('I', "DEV", "cpuidle: core %d: MPIDR %#llx, PMU status %#x, INT_EN %#x, CPU_INFORM %#x", cpu,
		    mpidr & ~(1ULL << 63), pmu_read(pmu_alive, pmu_int_en(cpu) - 0x40), pmu_read(pmu_alive, pmu_int_en(cpu)),
		    pmu_read(pmu_alive, PMU_CPU_INFORM(cpu)));
		if (gic_v3_spi_attach_to(MCT_INTID(cpu), false, mct_interrupt, (void *)(uintptr_t)cpu, cpu) != KERN_SUCCESS) {
			m2m_klog('E', "DEV", "cpuidle: core %d's MCT interrupt %u could not be attached: not armed", cpu,
			    MCT_INTID(cpu));
			return;
		}
		cores++;
	}
	__builtin_arm_dsb(DSB_SY);
	os_atomic_store(&redwood_cpuidle_on, true, release);
	m2m_klog('I', "DEV", "cpuidle: armed on %u cores: PSCI CPU_SUSPEND %#x through the loader's trampoline %#llx, "
	    "woken by the MCT's local timers", cores, PSCI_C2_POWER_DOWN, resume_tramp_pa);
}

void redwood_cpuidle_init(void);
void
redwood_cpuidle_init(void)
{
	DTEntry chosen;
	void const *prop;
	unsigned int size;
	uint32_t on = 0, after = 60;
	uint64_t deadline;

	if (!PE_parse_boot_argn("cpuidle", &on, sizeof(on)) || on != 1) {
		return;
	}
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "psci-cpu-resume-trampoline", &prop, &size) != kSuccess || size != 8) {
		return;         /* a loader without kernel_cpu_resume */
	}
	memcpy(&resume_tramp_pa, prop, 8);
	if (SecureDTGetProperty(chosen, "psci-cpu-resume-data", &prop, &size) != kSuccess || size != 8) {
		return;
	}
	memcpy(&resume_data_pa, prop, 8);
	PE_parse_boot_argn("cpuidle_after", &after, sizeof(after));
	clock_interval_to_deadline(after, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(thread_call_allocate(redwood_cpuidle_arm, NULL), deadline);
}

/*
 * From cpu_idle() in place of WFI, interrupts masked: false when not worth it (WFI then); true when the
 * firmware came back without powering the core off (an interrupt was already pending). When it does power
 * off, this does not return: the core comes back through redwood_cpuidle_resumed().
 */
bool redwood_cpuidle_enter(void);
bool
redwood_cpuidle_enter(void)
{
	cpu_data_t *cdp = getCpuDatap();
	int cpu = cpu_number();
	uint64_t now, pop, left, ticks;
	int64_t status;

	if (!os_atomic_load(&redwood_cpuidle_on, acquire) || cpu >= 8 || latency_abs[cpu] == 0) {
		return false;
	}
	now = mach_absolute_time();
	pop = cdp->rtcPop;
	left = pop > now ? pop - now : 0;
	if (left < residency_abs[cpu]) {
		redwood_cpuidle_counts[cpu].skipped++;
		return false;
	}
	/* the MCT local timer counts 13 MHz (26 MHz oscillator / (TCNTB 1 + 1)); woken that much before the deadline */
	absolutetime_to_nanoseconds(left - latency_abs[cpu], &ticks);
	ticks = ticks / 1000 * 13;
	mct_start((unsigned)cpu, (uint32_t)(ticks > 0x7fffffff ? 0x7fffffff : ticks));
	redwood_cpuidle_counts[cpu].entered++;
	suspend_at[cpu] = now;
	pmu_core((unsigned)cpu, true);
	status = psci_call(PSCI_CPU_SUSPEND, PSCI_C2_POWER_DOWN, resume_tramp_pa, 0);
	/* back without a power-down */
	pmu_core((unsigned)cpu, false);
	mct_stop((unsigned)cpu);
	if (status != 0) {
		redwood_cpuidle_counts[cpu].refused++;
		if (status == -1 || status == -2) {     /* not supported, invalid parameters: never again */
			os_atomic_store(&redwood_cpuidle_on, false, release);
		}
	} else {
		redwood_cpuidle_counts[cpu].returned++;
	}
	return true;
}

/* A core back from a power-down (arm_init_idle_cpu, interrupts masked): what it lost, then XNU's own path. */
void redwood_cpuidle_resumed(void);
void
redwood_cpuidle_resumed(void)
{
	int cpu = cpu_number();

	gic_v3_irq_cpu_resume();
	if (redwood_jit_flush_on) {
		redwood_uci_off(NULL);
	}
	if (cpu < 8 && mct != NULL) {
		pmu_core((unsigned)cpu, false);
		mct_stop((unsigned)cpu);
		redwood_cpuidle_counts[cpu].resumed++;
		redwood_cpuidle_resumed_at[cpu] = mach_absolute_time();
		redwood_cpuidle_counts[cpu].slept_abs += mach_absolute_time() - suspend_at[cpu];
	}
	getCpuDatap()->rtcPop = EndOfAllTime;   /* the comparator is gone: whatever was set is set again */
	timer_resync_deadlines();
}

/*
 * What wakes the cores (with cpuidle armed): per core, its interrupts by kind and the timer callouts that
 * fired on it, by function, and for a thread's own timeout (thread_timer_expire: a sleep, a wait with a
 * deadline) by process and thread. Reported every 10 s from the dvfs: report (redwood_wake_report). Each
 * core writes only its own table, from interrupt context; the reader takes the counts with an exchange.
 */
#define WAKE_SLOTS      24
struct wake_slot {
	uintptr_t key;                  /* the thread for thread_timer_expire, else the function */
	uintptr_t func;
	uint32_t count;
	int pid;
	char name[40];                  /* process / thread */
	uint64_t frames[6];             /* an unnamed kernel thread's: where it waits (return addresses, unslid) */
	bool told;
};
static struct wake_slot wake_timers[8][WAKE_SLOTS];
static uint32_t wake_irqs[8][4];        /* timer PPI, SGI (another core), SPI (a device), other */
static struct {
	uint32_t intid, count;
} wake_spis[8][6];
extern void thread_timer_expire(void *p0, void *p1);
extern vm_offset_t kernel_stack_size;
extern char *proc_name_address(void *p);
extern int proc_pid(void *p);
extern void bsd_getthreadname(void *uth, char *buffer);

void redwood_wake_irq(int cpu, uint32_t intid);
void
redwood_wake_irq(int cpu, uint32_t intid)
{
	if (!os_atomic_load(&redwood_cpuidle_on, relaxed) || cpu >= 8) {
		return;
	}
	if (intid == 27) {
		wake_irqs[cpu][0]++;
	} else if (intid < 16) {
		wake_irqs[cpu][1]++;
	} else if (intid >= 32 && intid < 1020) {
		wake_irqs[cpu][2]++;
		for (unsigned i = 0; i < 6; i++) {
			if (wake_spis[cpu][i].intid == intid || wake_spis[cpu][i].count == 0) {
				wake_spis[cpu][i].intid = intid;
				os_atomic_inc(&wake_spis[cpu][i].count, relaxed);
				break;
			}
		}
	} else {
		wake_irqs[cpu][3]++;
	}
}

void redwood_wake_timer(void *func, void *param0);
void
redwood_wake_timer(void *func, void *param0)
{
	int cpu = cpu_number();
	bool thread_timer = func == (void *)thread_timer_expire;
	uintptr_t key = thread_timer ? (uintptr_t)param0 : (uintptr_t)func;
	struct wake_slot *free_slot = NULL;

	if (!os_atomic_load(&redwood_cpuidle_on, relaxed) || cpu >= 8) {
		return;
	}
	for (unsigned i = 0; i < WAKE_SLOTS; i++) {
		struct wake_slot *w = &wake_timers[cpu][i];

		if (w->key == key) {
			os_atomic_inc(&w->count, relaxed);
			return;
		}
		if (free_slot == NULL && w->count == 0) {
			free_slot = w;
		}
	}
	if (free_slot == NULL) {
		return;
	}
	free_slot->key = 0;
	free_slot->func = (uintptr_t)func;
	free_slot->pid = -1;
	free_slot->name[0] = 0;
	if (thread_timer && param0 != NULL) {
		thread_t thread = (thread_t)param0;
		void *proc = get_bsdtask_info(get_threadtask(thread));
		char tname[64] = "";

		free_slot->pid = proc != NULL ? proc_pid(proc) : 0;
		bsd_getthreadname(get_bsdthread_info(thread), tname);
		snprintf(free_slot->name, sizeof(free_slot->name), "%s/%s", proc != NULL ? proc_name_address(proc) : "kernel",
		    tname[0] ? tname : "-");
		bzero(free_slot->frames, sizeof(free_slot->frames));
		free_slot->told = false;
		if (tname[0] == 0 && thread->kernel_stack != 0 && thread->machine.kstackptr != 0) {
			/* it is blocked (its timeout is firing): its saved frame pointer chain, within its own stack */
			vm_offset_t lo = thread->kernel_stack, hi = lo + kernel_stack_size;
			arm_kernel_context_t *ctx = (arm_kernel_context_t *)thread->machine.kstackptr;
			uint64_t fp = ctx->ss.fp;

			free_slot->frames[0] = VM_KERNEL_UNSLIDE(ctx->ss.lr);
			for (unsigned f = 1; f < 6 && fp >= lo && fp + 16 <= hi && (fp & 7) == 0; f++) {
				free_slot->frames[f] = VM_KERNEL_UNSLIDE(((uint64_t *)fp)[1]);
				fp = ((uint64_t *)fp)[0];
			}
		}
	}
	os_atomic_store(&free_slot->count, 1, relaxed);
	os_atomic_store(&free_slot->key, key, release);
}

void redwood_wake_report(void);
void
redwood_wake_report(void)
{
	extern void m2m_klog(char sev, const char *cat, const char *fmt, ...);

	if (!os_atomic_load(&redwood_cpuidle_on, relaxed)) {
		return;
	}
	for (int cpu = 0; cpu < 8; cpu++) {
		char line[900];
		size_t at = 0;
		uint32_t counts[WAKE_SLOTS];
		uint32_t irq[4], spi_total = 0;

		for (unsigned k = 0; k < 4; k++) {
			irq[k] = os_atomic_xchg(&wake_irqs[cpu][k], 0, relaxed);
		}
		at += snprintf(line + at, sizeof(line) - at, "wakes core %d, 10 s: timer %u, from other cores %u, devices %u",
		    cpu, irq[0], irq[1], irq[2]);
		for (unsigned i = 0; i < 6; i++) {
			uint32_t c = os_atomic_xchg(&wake_spis[cpu][i].count, 0, relaxed);

			if (c != 0 && at < sizeof(line)) {
				at += snprintf(line + at, sizeof(line) - at, "%s%u:%u", spi_total == 0 ? " (" : " ",
				    wake_spis[cpu][i].intid, c);
				spi_total += c;
			}
		}
		if (spi_total != 0 && at < sizeof(line)) {
			at += snprintf(line + at, sizeof(line) - at, ")");
		}
		for (unsigned i = 0; i < WAKE_SLOTS; i++) {
			counts[i] = os_atomic_xchg(&wake_timers[cpu][i].count, 0, relaxed);
		}
		if (at < sizeof(line)) {
			at += snprintf(line + at, sizeof(line) - at, "; callouts:");
		}
		/* the eight busiest */
		for (unsigned n = 0; n < 8; n++) {
			unsigned best = WAKE_SLOTS;

			for (unsigned i = 0; i < WAKE_SLOTS; i++) {
				if (counts[i] != 0 && (best == WAKE_SLOTS || counts[i] > counts[best])) {
					best = i;
				}
			}
			if (best == WAKE_SLOTS || at >= sizeof(line)) {
				break;
			}
			struct wake_slot *w = &wake_timers[cpu][best];

			if (w->pid >= 0) {
				at += snprintf(line + at, sizeof(line) - at, " %u %s[%d]", counts[best], w->name, w->pid);
				if (w->frames[0] != 0 && at < sizeof(line)) {
					at += snprintf(line + at, sizeof(line) - at, "#%d.%u", cpu, best);
					if (!w->told) {
						w->told = true;
						m2m_klog('I', "PERF", "wakes: thread #%d.%u waits at %#llx %#llx %#llx %#llx %#llx %#llx "
						    "(unslid return addresses, innermost first)", cpu, best, w->frames[0], w->frames[1],
						    w->frames[2], w->frames[3], w->frames[4], w->frames[5]);
					}
				}
			} else {
				at += snprintf(line + at, sizeof(line) - at, " %u fn %#lx", counts[best],
				    (unsigned long)VM_KERNEL_UNSLIDE(w->func));
			}
			counts[best] = 0;
		}
		if (irq[0] + irq[1] + irq[2] != 0) {
			m2m_klog('I', "PERF", "%s", line);
		}
	}
}

#endif /* defined(GENERIC_PSCI_SMP) */
