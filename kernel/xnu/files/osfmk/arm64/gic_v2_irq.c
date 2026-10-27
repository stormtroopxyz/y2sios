/*
 * Minimal GICv2 (ARM GIC-400) support for generic (non-Apple) ARMv8 boards.
 *
 * This is the counterpart of osfmk/arm64/gic_v3_irq.c for boards whose interrupt
 * controller is a GICv2 rather than a GICv3. The Samsung Exynos 990 (Galaxy S20+
 * 5G, "y2s") is one: its device tree describes the controller as
 * "arm,cortex-a15-gic" with four windows (GICD/GICC/GICH/GICV):
 *
 *   /interrupt-controller@10100000 {
 *       compatible = "arm,cortex-a15-gic", "arm,cortex-a9-gic";
 *       reg = <0 0x10101000 0x1000>,   // distributor
 *             <0 0x10102000 0x1000>,   // CPU interface (banked per processor)
 *             <0 0x10104000 0x2000>,   // hypervisor (unused: we are at EL1)
 *             <0 0x10106000 0x2000>;   // virtual CPU interface (unused)
 *   };
 *
 * A board compiles exactly one of gic_v3_irq.c / gic_v2_irq.c, chosen by the
 * GENERIC_GIC_V3_IRQ / GENERIC_GIC_V2_IRQ board defines. To keep every caller
 * (pecpert/arm/pe_fiq.c, osfmk/arm64/psci_smp.c, osfmk/arm64/loader_return.c,
 * iokit/Kernel/GenericARMPE.cpp, bsd/dev/redwood_lx.c) unchanged, this file
 * deliberately exports the SAME names the GICv3 driver does ("gic_v3_*"); the
 * "v3" in the names is historical. A follow-up should rename the whole surface
 * to gic_irq_* once both drivers are known good.
 *
 * Differences from the GICv3 driver that matter here:
 *   - the CPU interface is MMIO (GICC), not the ICC_*_EL1 system registers
 *   - interrupts are acknowledged from GICC_IAR and ended with GICC_EOIR
 *     (no separate deactivate: we run with EOImode = 0)
 *   - software-generated interrupts are sent through the distributor
 *     (GICD_SGIR, a target bitmask of CPU interfaces) instead of ICC_SGI1R_EL1
 *   - a shared peripheral interrupt is routed by writing its target CPU mask
 *     (GICD_ITARGETSR) instead of GICD_IROUTER
 *   - there is no redistributor: the SGI/PPI bank lives in the distributor
 *     (GICD_ISENABLER0 etc.), banked by the hardware per processor
 *   - there is no affinity routing: the target is a "CPU interface number",
 *     which for GICv2 is SoC-wired and not derivable from MPIDR. It is found
 *     by reading an SGI/PPI target register (see gic_cpuif_self()).
 *
 * Live in this file: Group 1 (IRQ), the EL1 virtual timer PPI (27), the two
 * SGI signals between processors (0 and 1), and the shared interrupts a driver
 * attaches (gic_v3_spi_attach()).
 */
#include <mach/mach_types.h>
#include <kern/debug.h>
#include <kern/simple_lock.h>
#include <os/atomic_private.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_internal.h>
#include <arm/machine_cpu.h>
#include <arm/machine_routines.h>
#include <arm/rtclock.h>
#include <arm64/psci_smp.h>
#include <arm64/proc_reg.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <pexpert/arm64/board_config.h>

#if defined(GENERIC_GIC_V2_IRQ)

/* Distributor (GICD); offsets from the mapped base. */
#define GICD_CTLR               0x0000
#define GICD_CTLR_ENABLE        0x0001      /* non-secure view: bit 0 is EnableGrp1 */
#define GICD_TYPER              0x0004
#define GICD_IGROUPR(n)         (0x0080 + 4 * (n))
#define GICD_ISENABLER(n)       (0x0100 + 4 * (n))
#define GICD_ICENABLER(n)       (0x0180 + 4 * (n))
#define GICD_ISPENDR(n)         (0x0200 + 4 * (n))
#define GICD_ICPENDR(n)         (0x0280 + 4 * (n))
#define GICD_ISACTIVER(n)       (0x0300 + 4 * (n))
#define GICD_ICACTIVER(n)       (0x0380 + 4 * (n))
#define GICD_IPRIORITYR         0x0400      /* one byte per interrupt */
#define GICD_ITARGETSR          0x0800      /* one byte per interrupt (SPI: RW mask; SGI/PPI: RO) */
#define GICD_ICFGR(n)           (0x0c00 + 4 * (n))
#define GICD_SGIR               0x0f00
#define GICD_SGIR_NSATTDR       (1u << 15)  /* must be written 0 in the non-secure view */

/* CPU interface (GICC); banked: each processor sees its own at this address. */
#define GICC_CTLR               0x0000
#define GICC_CTLR_ENABLE_GRP1   0x0001
#define GICC_CTLR_ACKCTL        (1u << 2)
#define GICC_PMR                0x0004
#define GICC_BPR                0x0008
#define GICC_IAR                0x000c
#define GICC_EOIR               0x0010
#define GICC_IIDR               0x00fc

#define GIC_SGI_SIGNAL          0           /* cpu_signal(): another processor wants this one's attention */
#define GIC_SGI_STOP            1           /* psci_smp_stop_here(): the system is going back to the boot loader */
#define GIC_INTID_VTIMER        27          /* EL1 virtual timer PPI */
#define GIC_INTID_SPI_MIN       32
#define GIC_INTID_SPURIOUS      1023        /* GICv2: "end of interrupts for this processor" (0x3ff) */
#define GIC_SPI_MOST            48          /* attached at a time */
#define GIC_PRIORITY_DEFAULT    0xa0

static vm_offset_t gicd_base, gicc_base;

/*
 * The GICv2 "CPU interface number" of each processor (the value SGIR and
 * ITARGETSR speak). It is not the MPIDR: the SoC wires it. Each processor
 * discovers its own at bring-up (gic_v3_irq_cpu_init) and records it, so a signal
 * addressed to another processor's MPIDR can be translated here.
 */
static uint32_t gic_cpuif[MAX_CPUS];        /* by cpu_number() */
static uint32_t gic_cpuif_mpidr[MAX_CPUS];  /* by cpu_number(): the MPIDR it was found on */
static bool     gic_cpuif_seen[MAX_CPUS];   /* MPIDR 0 is a valid one, so "seen" needs its own flag */
static uint32_t gic_cpuif_count;

static uint32_t gic_active_iar[MAX_CPUS];   /* acknowledged, not yet EOI'd; 0: none */
uint64_t        gic_unhandled_count;
uint32_t        gic_last_unhandled;

/*
 * The attached SPIs, exactly as in the GICv3 driver: a slot is filled before
 * its interrupt is enabled and emptied after it has been disabled, so the
 * interrupt path reads the table without a lock.
 */
struct gic_spi {
	uint32_t intid;                     /* 0: a free slot */
	void   (*handler)(void *context, uint32_t intid);
	void    *context;
	uint32_t busy;                      /* handlers of it running now */
	uint64_t count;
};
static struct gic_spi gic_spi[GIC_SPI_MOST];
static decl_simple_lock_data(, gic_spi_lock);

void gic_v3_irq_cpu_init(void);
void gic_v3_irq_quiesce(void);
kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
void gic_v3_spi_mask(uint32_t intid);
void gic_v3_spi_unmask(uint32_t intid);
void gic_v3_spi_detach(uint32_t intid);
uint64_t gic_v3_spi_count(uint32_t intid);
void gic_v3_irq_cpu_shutdown(void);
void gic_v3_irq_signal(uint32_t phys_id);
void gic_v3_irq_signal_stop(uint32_t phys_id);

static inline uint32_t
gic_read32(vm_offset_t addr)
{
	return *(volatile uint32_t *)addr;
}

static inline void
gic_write32(vm_offset_t addr, uint32_t value)
{
	*(volatile uint32_t *)addr = value;
}

static inline uint8_t
gic_read8(vm_offset_t addr)
{
	return *(volatile uint8_t *)addr;
}

static inline void
gic_write8(vm_offset_t addr, uint8_t value)
{
	*(volatile uint8_t *)addr = value;
}

/*
 * The CPU interface number of the processor running this. GICv2 has no
 * register that names it (that arrives with the GICv3 redistributor's
 * GICR_TYPER), so it is read the way U-Boot and Linux do it on GICv2: the
 * ITARGETSR of a banked interrupt (an SGI or a PPI) reads back, for the
 * reading processor, a mask with that processor's CPU interface bit set.
 * Where the SoC does not answer (0), fall back to the linear order in which
 * the processors are numbered, which is the usual wiring.
 */
static uint32_t
gic_cpuif_self(void)
{
	uint8_t mask = gic_read8(gicd_base + GICD_ITARGETSR + GIC_INTID_VTIMER);

	if (mask != 0) {
		return (uint32_t)__builtin_ctz(mask);
	}
	return (uint32_t)cpu_number();
}

/* The CPU interface number to address a processor with that MPIDR. */
static uint32_t
gic_cpuif_of_phys(uint32_t phys_id)
{
	for (unsigned i = 0; i < MAX_CPUS; i++) {
		if (gic_cpuif_seen[i] && gic_cpuif_mpidr[i] == phys_id) {
			return gic_cpuif[i];
		}
	}
	/*
	 * Not seen yet (it has not run gic_v3_irq_cpu_init()). On Exynos the CPU
	 * interface is wired in the order the device tree lists the cores, so the
	 * linear index is a better guess than the raw MPIDR. VERIFY on hardware.
	 */
	return phys_id & 0x7;
}

static void
gic_mask(uint32_t intid)
{
	if (intid < 32) {
		gic_write32(gicd_base + GICD_ICENABLER(0), 1u << intid);        /* banked per processor */
	} else {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A software-generated interrupt for one processor. GICD_SGIR (GICv2):
 *   [3:0]   the SGI to send
 *   [15]    NSATTDR: 0 for a non-secure target (we are non-secure at EL1)
 *   [23:16] target list, when the filter is 0b00
 *   [25:24] target list filter: 0b00 list, 0b01 all but self, 0b10 self only
 * Unlike ICC_SGI1R_EL1 this cannot name an MPIDR: it names CPU interfaces.
 */
static void
gic_send_sgi_to_cpuif(uint32_t cpuif, uint32_t sgi)
{
	gic_write32(gicd_base + GICD_SGIR, (uint32_t)sgi | ((1u << (cpuif & 0x7)) << 16));
	__builtin_arm_dsb(DSB_SY);
}

void
gic_v3_irq_signal(uint32_t phys_id)
{
	gic_send_sgi_to_cpuif(gic_cpuif_of_phys(phys_id), GIC_SGI_SIGNAL);
}

void
gic_v3_irq_signal_stop(uint32_t phys_id)
{
	gic_send_sgi_to_cpuif(gic_cpuif_of_phys(phys_id), GIC_SGI_STOP);
}

/* An SPI that a driver has attached: its handler called. false: nobody has it. */
static bool
gic_spi_handle(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		struct gic_spi *spi = &gic_spi[i];

		if (os_atomic_load(&spi->intid, acquire) == intid) {
			os_atomic_inc(&spi->busy, acquire);
			if (os_atomic_load(&spi->intid, relaxed) == intid) {
				spi->count++;
				spi->handler(spi->context, intid);
			}
			os_atomic_dec(&spi->busy, release);
			return true;
		}
	}
	return false;
}

/* GICv2 has no RWP bit; kept so the call sites read the same as the v3 driver. */
static void
gic_distributor_settle(void)
{
	__builtin_arm_dsb(DSB_SY);
}

void
gic_v3_spi_mask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS) {
		gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		gic_distributor_settle();
	}
}

void
gic_v3_spi_unmask(uint32_t intid)
{
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS) {
		gic_write32(gicd_base + GICD_ISENABLER(intid / 32), 1u << (intid % 32));
	}
}

/*
 * A driver takes an SPI: edge- or level-triggered, Group 1, the default
 * priority, targeted at the processor this runs on, and enabled. The handler
 * runs in interrupt context, before the interrupt is ended; a level-triggered
 * one has to quiet the source or mask it (gic_v3_spi_mask()). Thread context.
 */
kern_return_t gic_v3_spi_attach_to(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context, int cpu);

kern_return_t
gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context)
{
	return gic_v3_spi_attach_to(intid, edge, handler, context, -1);
}

/* ... targeted at processor `cpu` (cpu_number(); -1: this one) instead: a per-core timer (psci_smp.c's cpuidle) */
kern_return_t
gic_v3_spi_attach_to(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context,
    int cpu)
{
	uint32_t cpuif;
	struct gic_spi *slot = NULL;

	if (cpu < 0) {
		cpuif = gic_cpuif_self();
	} else if (cpu < MAX_CPUS && gic_cpuif_seen[cpu]) {
		cpuif = gic_cpuif[cpu];
	} else {
		return KERN_INVALID_ARGUMENT;
	}
	uint32_t config;

	if (gicd_base == 0 || intid < GIC_INTID_SPI_MIN || intid >= GIC_INTID_SPURIOUS || handler == NULL) {
		return KERN_INVALID_ARGUMENT;
	}
	simple_lock(&gic_spi_lock, LCK_GRP_NULL);
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			simple_unlock(&gic_spi_lock);
			return KERN_NAME_EXISTS;
		}
		if (slot == NULL && gic_spi[i].intid == 0 && gic_spi[i].busy == 0) {
			slot = &gic_spi[i];
		}
	}
	if (slot == NULL) {
		simple_unlock(&gic_spi_lock);
		return KERN_RESOURCE_SHORTAGE;
	}
	slot->handler = handler;
	slot->context = context;
	slot->count = 0;
	os_atomic_store(&slot->intid, intid, release);
	simple_unlock(&gic_spi_lock);

	gic_v3_spi_mask(intid);
	gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	gic_write32(gicd_base + GICD_ICACTIVER(intid / 32), 1u << (intid % 32));
	/* Group 1 (non-secure) so it is delivered as an IRQ, not an FIQ */
	gic_write32(gicd_base + GICD_IGROUPR(intid / 32), gic_read32(gicd_base + GICD_IGROUPR(intid / 32)) | 1u << (intid % 32));
	gic_write8(gicd_base + GICD_IPRIORITYR + intid, GIC_PRIORITY_DEFAULT);
	config = gic_read32(gicd_base + GICD_ICFGR(intid / 16)) & ~(3u << (intid % 16 * 2));
	gic_write32(gicd_base + GICD_ICFGR(intid / 16), config | (edge ? 2u : 0u) << (intid % 16 * 2));
	/* GICD_ITARGETSR: send it to the processor that attaches it. */
	gic_write8(gicd_base + GICD_ITARGETSR + intid, (uint8_t)(1u << (cpuif & 7)));
	gic_distributor_settle();
	gic_v3_spi_unmask(intid);
	return KERN_SUCCESS;
}

/* ... and gives it back: disabled, nothing pending, no handler of it running when this returns. */
void
gic_v3_spi_detach(uint32_t intid)
{
	gic_v3_spi_mask(intid);
	if (gicd_base != 0 && intid >= GIC_INTID_SPI_MIN && intid < GIC_INTID_SPURIOUS) {
		gic_write32(gicd_base + GICD_ICPENDR(intid / 32), 1u << (intid % 32));
	}
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			os_atomic_store(&gic_spi[i].intid, 0, release);
			while (os_atomic_load(&gic_spi[i].busy, acquire) != 0) {
				/* a handler of it on another processor is finishing */
			}
		}
	}
}

/* How many have arrived since it was attached. */
uint64_t
gic_v3_spi_count(uint32_t intid)
{
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		if (gic_spi[i].intid == intid) {
			return gic_spi[i].count;
		}
	}
	return 0;
}

static void
gic_irq_handler(__unused void *target, __unused void *refCon, __unused void *nub, __unused int source)
{
	int cpu = cpu_number();

	for (;;) {
		uint32_t iar = gic_read32(gicc_base + GICC_IAR);
		uint32_t intid = iar & 0x3ff;

		if (intid == GIC_INTID_SPURIOUS) {
			break;
		}
		gic_active_iar[cpu] = iar;
		{
			extern void redwood_wake_irq(int cpu, uint32_t intid);

			redwood_wake_irq(cpu, intid);   /* what wakes the cores (psci_smp.c) */
		}

		if (intid == GIC_INTID_VTIMER) {
			/* same sequence as the FIQ path in sleh_fiq() */
			getCpuDatap()->cpu_decrementer = -1; /* Large */
			rtclock_intr(TRUE);
		} else if (intid == GIC_SGI_SIGNAL) {
			cpu_signal_handler();
		} else if (intid == GIC_SGI_STOP) {
			/* comes back only if nobody has in fact asked for that */
			psci_smp_stop_here();
		} else if (intid >= GIC_INTID_SPI_MIN && gic_spi_handle(intid)) {
			/* a driver's */
		} else {
			/* nobody owns this interrupt yet: keep it from storming */
			gic_mask(intid);
			gic_unhandled_count++;
			gic_last_unhandled = intid;
		}

		gic_write32(gicc_base + GICC_EOIR, iar);
		__builtin_arm_dsb(DSB_SY);
		gic_active_iar[cpu] = 0;
	}
}

/*
 * Leave the controller in a state another environment can take over from:
 * finish any interrupt this processor acknowledged but did not complete.
 * Used on the way back to the boot loader (loader_return.c) and by a processor
 * about to be switched off for it (psci_smp.c).
 */
void
gic_v3_irq_quiesce(void)
{
	int cpu;

	if (gicd_base == 0) {
		return;
	}
	cpu = cpu_number();
	for (unsigned i = 0; i < GIC_SPI_MOST; i++) {
		uint32_t intid = gic_spi[i].intid;

		if (intid != 0) {
			gic_write32(gicd_base + GICD_ICENABLER(intid / 32), 1u << (intid % 32));
		}
	}
	if (gic_active_iar[cpu] != 0) {
		gic_write32(gicc_base + GICC_EOIR, gic_active_iar[cpu]);
		__builtin_arm_dsb(DSB_SY);
		gic_active_iar[cpu] = 0;
	}
}

/*
 * This processor is leaving for good. Nothing may be left that could interrupt
 * it: the banked SGI/PPI enables and any pending state are cleared, and the
 * CPU interface is turned off so the next kernel started here is not handed a
 * stale interrupt.
 */
void
gic_v3_irq_cpu_shutdown(void)
{
	if (gicd_base == 0) {
		return;
	}
	gic_v3_irq_quiesce();
	gic_write32(gicd_base + GICD_ICENABLER(0), 0xffffffffu);       /* its SGIs and PPIs */
	gic_write32(gicd_base + GICD_ICPENDR(0), 0xffffffffu);
	gic_write32(gicd_base + GICD_ICACTIVER(0), 0xffffffffu);
	gic_write32(gicc_base + GICC_CTLR, 0);
	__builtin_arm_dsb(DSB_SY);
}

/*
 * A core back from a power-down (PSCI CPU_SUSPEND: psci_smp.c's cpuidle). As Linux's GICv2 cpu_pm exit
 * (irq-gic.c gic_cpu_restore + gic_cpu_if_up): its CPU interface and its banked SGI/PPI setup are written
 * again, in case the power-down took them. What they read before that is kept for the record (the first
 * time on each core): whether they had in fact been lost.
 */
uint32_t gic_resume_seen[MAX_CPUS][2];  /* GICC_CTLR << 16 | GICC_PMR, GICD_ISENABLER0; bit 31 of [0]: set */
void gic_v3_irq_cpu_resume(void);
void
gic_v3_irq_cpu_resume(void)
{
	int cpu = cpu_number();
	uint32_t mine = 1u << GIC_INTID_VTIMER;

	if (gicd_base == 0) {
		return;
	}
	if (!(gic_resume_seen[cpu][0] & (1u << 31))) {
		gic_resume_seen[cpu][1] = gic_read32(gicd_base + GICD_ISENABLER(0));
		gic_resume_seen[cpu][0] = (1u << 31) | (gic_read32(gicc_base + GICC_CTLR) & 0x7fff) << 16 |
		    (gic_read32(gicc_base + GICC_PMR) & 0xffff);
	}
	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write8(gicd_base + GICD_IPRIORITYR + GIC_SGI_SIGNAL, GIC_PRIORITY_DEFAULT);
		gic_write8(gicd_base + GICD_IPRIORITYR + GIC_SGI_STOP, GIC_PRIORITY_DEFAULT);
		mine |= sgis;
	}
	gic_write32(gicd_base + GICD_IGROUPR(0), gic_read32(gicd_base + GICD_IGROUPR(0)) | mine);
	gic_write8(gicd_base + GICD_IPRIORITYR + GIC_INTID_VTIMER, GIC_PRIORITY_DEFAULT);
	gic_write32(gicd_base + GICD_ISENABLER(0), mine);
	gic_write32(gicc_base + GICC_PMR, 0xff);
	gic_write32(gicc_base + GICC_BPR, 0);
	gic_write32(gicc_base + GICC_CTLR, GICC_CTLR_ENABLE_GRP1);
	__builtin_arm_dsb(DSB_SY);
}

/* Per-CPU bring-up: every processor calls this for itself, the boot processor first. */
void
gic_v3_irq_cpu_init(void)
{
	int cpu = cpu_number();
	uint32_t mine = 1u << GIC_INTID_VTIMER;
	uint32_t cpuif;

	if (gicd_base == 0) {
		DTEntry entry;
		void const *prop;
		unsigned int prop_size;

		if (SecureDTLookupEntry(NULL, "/arm-io/gic", &entry) != kSuccess ||
		    SecureDTGetProperty(entry, "reg", &prop, &prop_size) != kSuccess ||
		    prop_size < 4 * sizeof(uint64_t)) {
			panic("gic: no usable /arm-io/gic node in the device tree");
		}
		uint64_t const *reg = prop;
		vm_offset_t soc_base = pe_arm_get_soc_base_phys();

		simple_lock_init(&gic_spi_lock, 0);
		/* reg = <distributor offset, size, cpu interface offset, size> */
		gicd_base = ml_io_map(soc_base + reg[0], reg[1]);
		gicc_base = ml_io_map(soc_base + reg[2], reg[3]);
		if (gicd_base == 0 || gicc_base == 0) {
			panic("gic: cannot map the distributor/cpu interface");
		}
		/*
		 * Distributor: Group 1 only, and no affinity routing (GICv2 has none).
		 * The non-secure view of GICD_CTLR has EnableGrp1 in bit 0.
		 */
		gic_write32(gicd_base + GICD_CTLR, GICD_CTLR_ENABLE);
		gic_distributor_settle();
		kprintf("gic: v2, distributor %p, cpu interface %p (IIDR 0x%08x)\n",
		    (void *)gicd_base, (void *)gicc_base, gic_read32(gicc_base + GICC_IIDR));
	} else if (gic_cpuif_seen[cpu]) {
		return;
	}

	/* CPU interface: all priorities, no grouping, Group 1 on, simple EOI (AckCtl = 0). */
	gic_write32(gicc_base + GICC_PMR, 0xff);
	gic_write32(gicc_base + GICC_BPR, 0);
	gic_write32(gicc_base + GICC_CTLR, GICC_CTLR_ENABLE_GRP1);
	__builtin_arm_dsb(DSB_SY);

	cpuif = gic_cpuif_self();
	gic_cpuif[cpu] = cpuif;
	/* the registered phys id (what gic_v3_irq_signal() is given), not MPIDR_EL1: on Exynos they differ */
	gic_cpuif_mpidr[cpu] = getCpuDatap()->cpu_phys_id;
	gic_cpuif_seen[cpu] = true;
	if (cpuif + 1 > gic_cpuif_count) {
		gic_cpuif_count = cpuif + 1;
	}

	/*
	 * The interrupts between processors, where there is more than one. A core
	 * that an earlier kernel switched off in the middle of one may still have
	 * it marked active and would then never see another.
	 */
	if (ml_get_cpu_count() > 1) {
		uint32_t sgis = (1u << GIC_SGI_SIGNAL) | (1u << GIC_SGI_STOP);

		gic_write32(gicd_base + GICD_ICACTIVER(0), sgis);
		gic_write32(gicd_base + GICD_ICPENDR(0), sgis);
		gic_write8(gicd_base + GICD_IPRIORITYR + GIC_SGI_SIGNAL, GIC_PRIORITY_DEFAULT);
		gic_write8(gicd_base + GICD_IPRIORITYR + GIC_SGI_STOP, GIC_PRIORITY_DEFAULT);
		gic_write32(gicd_base + GICD_IGROUPR(0), gic_read32(gicd_base + GICD_IGROUPR(0)) | sgis);
		mine |= sgis;
	}

	/* Virtual timer PPI (and the SGIs): Group 1, default priority, enabled (banked). */
	gic_write32(gicd_base + GICD_IGROUPR(0), gic_read32(gicd_base + GICD_IGROUPR(0)) | mine);
	gic_write8(gicd_base + GICD_IPRIORITYR + GIC_INTID_VTIMER, GIC_PRIORITY_DEFAULT);
	gic_write32(gicd_base + GICD_ISENABLER(0), mine);
	gic_distributor_settle();

	ml_install_interrupt_handler(NULL, 0, NULL, gic_irq_handler, NULL);
	kprintf("gic: cpu %d is CPU interface %d (MPIDR 0x%llx), virtual timer on PPI %d\n",
	    cpu, cpuif, (unsigned long long)__builtin_arm_rsr64("MPIDR_EL1"), GIC_INTID_VTIMER);
}

#endif /* GENERIC_GIC_V2_IRQ */
