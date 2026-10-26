/*
 * perf=1: what the processors of this phone really run at, and a line every minute about it.
 *
 * Each core's clock is measured without the performance counters (on Samsung phones a
 * hypervisor may trap them): a chain of dependent adds takes one cycle each on the A55, the
 * A76 and the Mongoose M5 alike, so cycles = adds, timed by the 26 MHz architectural timer.
 *
 *   perf: cpu 0 (mpidr 0x0, Cortex-A55): 1950 MHz
 *
 * Boot args: perf=1 (once at boot, then every perf_secs, default 60, to see throttling).
 */

#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern processor_t cpu_to_processor(int cpu);
extern processor_t thread_bind(processor_t processor);
extern wait_result_t thread_block(thread_continue_t continuation);
extern unsigned int real_ncpus;
extern uint32_t processor_avail_count;
extern int psci_smp_cpu_is_running(int cpu);
extern uint32_t vm_page_free_count;
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define ADDS_PER_LOOP   64
#define LOOPS           50000           /* 3.2 M cycles: ~1.6 ms at 2 GHz, ~6 ms at 500 MHz */

/* the kernel's own timebase (a direct CNTVCT_EL0 read gave nothing usable on this phone, 24 Oct) */
static inline uint64_t
counter(void)
{
	__asm__ volatile ("isb" ::: "memory");
	return mach_absolute_time();
}

static uint64_t
counter_freq(void)
{
	uint64_t ns = 0;

	absolutetime_to_nanoseconds(1000000000ULL, &ns);       /* ns in 1e9 ticks */
	return ns != 0 ? 1000000000ULL * 1000000000ULL / ns : 24000000;
}

#define ADD8 "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n" \
	     "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"

static uint64_t
measure_mhz(uint64_t *ticks)
{
	uint64_t best = 0;

	*ticks = 0;
	int round;

	for (round = 0; round < 3; round++) {
		uint64_t x = 0, n = LOOPS, t0, t1, mhz;
		/* with interrupts off: on a busy core the other threads' time would be counted as ours */
		boolean_t enabled = ml_set_interrupts_enabled(FALSE);

		t0 = counter();
		__asm__ volatile (
                        "1:\n"
                        ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8
                        "subs %1, %1, #1\n"
                        "b.ne 1b\n"
                        : "+r"(x), "+r"(n) : : "cc");
		t1 = counter();
		ml_set_interrupts_enabled(enabled);
		if (t1 > t0) {
			mhz = (uint64_t)ADDS_PER_LOOP * LOOPS * counter_freq() / (t1 - t0) / 1000000;
			if (mhz >= best) {      /* an interrupt can only make a round look slower */
				best = mhz;
				*ticks = t1 - t0;
			}
		}
	}
	return best;
}

static const char *
core_name(void)
{
	uint64_t midr;

	__asm__ volatile ("mrs %0, midr_el1" : "=r"(midr));
	switch ((midr >> 4) & 0xfff) {
	case 0xd05:
		return "Cortex-A55";
	case 0xd0b:
		return "Cortex-A76";
	default:
		return (midr >> 24) == 0x53 ? "Mongoose" : "unknown";
	}
}

/* Only the cores that are running: a thread bound to one that never started waits forever. */
static void
measure_all(const char *when, unsigned cores)
{
	char line[512];
	size_t used = 0;
	unsigned cpu;

	used += snprintf(line + used, sizeof(line) - used, "perf: %s:", when);
	for (cpu = 0; cpu < cores && cpu < 8; cpu++) {
		processor_t processor = cpu_to_processor((int)cpu);
		uint64_t mpidr, mhz, ticks;

		if (processor == PROCESSOR_NULL || !psci_smp_cpu_is_running((int)cpu)) {
			if (used < sizeof(line)) {
				used += snprintf(line + used, sizeof(line) - used, " cpu%u off;", cpu);
			}
			continue;
		}
		thread_bind(processor);
		thread_block(THREAD_CONTINUE_NULL);
		__asm__ volatile ("mrs %0, mpidr_el1" : "=r"(mpidr));
		mhz = measure_mhz(&ticks);
		if (used < sizeof(line)) {
			used += snprintf(line + used, sizeof(line) - used, " cpu%u(%#llx %s) %llu MHz (%llu t);", cpu,
			    mpidr & 0xffffff, core_name(), mhz, ticks);
		}
	}
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
	printf("%s free pages %u\n", line, vm_page_free_count);
}

static void
perf_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 60;
	uint64_t minutes = 0;

	unsigned cores, waited;

	PE_parse_boot_argn("perf_secs", &secs, sizeof(secs));
	if (secs < 5) {
		secs = 5;
	}
	/* the other cores are started one after the other, well into the boot */
	for (waited = 0; processor_avail_count < real_ncpus && waited < 180; waited++) {
		delay_for_interval(1, NSEC_PER_SEC);
	}
	cores = real_ncpus;
	printf("perf: %u of %u cores running after %u s; measuring %u\n", processor_avail_count, real_ncpus, waited, cores);
	measure_all("boot", cores);
	for (;;) {
		char when[32];

		delay_for_interval(secs, NSEC_PER_SEC);
		snprintf(when, sizeof(when), "+%llu s", ++minutes * secs);
		measure_all(when, cores);
	}
}

void
redwood_perf_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("perf", &on, sizeof(on)) || on == 0) {
		return;
	}
	if (kernel_thread_start(perf_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
