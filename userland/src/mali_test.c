/*
 * mali_test: the GPU through the interface programs have (iokit/Kernel/RedwoodMali.cpp, Linux's
 * panfrost), as Mesa will use it through tools/mesa/redwood-drm: parameters, a buffer made and
 * mapped, a job submitted with a syncobj to signal, the wait, and what the GPU wrote. Then many
 * jobs in a row, for how long one takes from submission to done.
 *
 *   mali_test            the checks, then 1000 jobs
 *   mali_test N          ... N jobs
 *
 * The jobs are WRITE_VALUE jobs (Mesa's src/panfrost/genxml/v9.xml): a 32-byte job header, then
 * the address to write, the kind of write and the value. No shader is involved.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct get_param { uint32_t param, pad; uint64_t value; };
struct create_bo { uint32_t size, flags, handle, pad; uint64_t offset; };
struct bo_offset { uint32_t handle, pad; uint64_t offset; };
struct submit {
	uint64_t jc, in_syncs;
	uint32_t in_sync_count, out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count, requirements;
};
struct syncobj_create { uint32_t handle, flags; };
struct syncobj_wait {
	uint64_t handles;
	int64_t timeout_nsec;
	uint32_t count_handles, flags, first_signaled, pad;
	uint64_t deadline_nsec;
};
struct wait_bo { uint32_t handle, pad; int64_t timeout_ns; };
struct counts { uint64_t jobs, faults, resets, last; };

static io_connect_t gpu;

static int
call(unsigned selector, void *data, size_t size)
{
	size_t out = size;
	kern_return_t kr = IOConnectCallStructMethod(gpu, selector, data, size, data, &out);

	return kr == KERN_SUCCESS ? 0 : (int)kr;
}

static uint64_t
param(uint32_t which)
{
	struct get_param p = { .param = which };

	return call(0x44, &p, sizeof(p)) == 0 ? p.value : ~0ULL;
}

static double
microseconds(uint64_t ticks)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)ticks * tb.numer / tb.denom / 1000.0;
}

/* one WRITE_VALUE job at `cpu` (GPU address `gpu_va`) that writes `value` (64 bits) to `target` */
static void
write_value_job(uint8_t *cpu, uint64_t target, uint64_t value)
{
	uint32_t *w = (uint32_t *)cpu;

	memset(cpu, 0, 64);
	w[4] = (2U << 1) | (1U << 16);          /* type WRITE_VALUE, index 1 */
	memcpy(cpu + 32, &target, 8);
	w[10] = 7;                              /* IMMEDIATE 64 */
	memcpy(cpu + 48, &value, 8);
}

int
main(int argc, char **argv)
{
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("RedwoodMali"));
	unsigned runs = argc > 1 ? (unsigned)atoi(argv[1]) : 1000;
	struct create_bo bo = { .size = 64 * 1024, .flags = 1 /* NOEXEC */ };
	struct bo_offset off;
	struct syncobj_create sync = { 0 };
	struct submit s;
	struct syncobj_wait wait;
	struct counts c;
	mach_vm_address_t map = 0;
	mach_vm_size_t mapped = 0;
	uint32_t handle, syncobj;
	uint64_t va, t0, t1, t_submit = 0, t_wait = 0;
	uint8_t *cpu;
	int bad = 0, kr;

	if (service == 0 || IOServiceOpen(service, mach_task_self(), 0, &gpu) != KERN_SUCCESS) {
		printf("mali_test: no RedwoodMali service (boot-arg mali=1?)\n");
		return 1;
	}
	printf("mali_test: product %#llx revision %#llx shader cores %#llx tiler %#llx L2 %#llx AS %#llx JS %#llx\n", param(0),
	    param(1), param(2), param(3), param(4), param(6), param(7));
	printf("mali_test: max threads %llu, TLS alloc %llu, texture features %#llx, timestamp %llu at %llu Hz, coherency %llu\n",
	    param(14), param(39), param(18), param(41), param(42), param(44));

	if ((kr = call(0x42, &bo, sizeof(bo))) != 0) {
		printf("mali_test: CREATE_BO failed: %#x\n", kr);
		return 1;
	}
	handle = bo.handle;
	off.handle = handle;
	if (call(0x45, &off, sizeof(off)) != 0 || off.offset != bo.offset) {
		printf("mali_test: GET_BO_OFFSET disagrees: %#llx vs %#llx\n", off.offset, bo.offset);
		bad++;
	}
	va = bo.offset;
	if ((kr = IOConnectMapMemory64(gpu, handle, mach_task_self(), &map, &mapped, kIOMapAnywhere)) != KERN_SUCCESS) {
		printf("mali_test: mapping buffer %u failed: %#x\n", handle, kr);
		return 1;
	}
	cpu = (uint8_t *)(uintptr_t)map;
	printf("mali_test: buffer %u: %llu bytes at GPU %#llx, here %p\n", handle, mapped, va, cpu);
	if (call(0xbf, &sync, sizeof(sync)) != 0) {
		printf("mali_test: SYNCOBJ_CREATE failed\n");
		return 1;
	}
	syncobj = sync.handle;

	/* 1: one job, a syncobj to signal, the wait */
	write_value_job(cpu, va + 0x8000, 0x0123456789abcdefULL);
	memset(cpu + 0x8000, 0, 8);
	memset(&s, 0, sizeof(s));
	s.jc = va;
	s.out_sync = syncobj;
	s.bo_handles = (uint64_t)(uintptr_t)&handle;
	s.bo_handle_count = 1;
	if ((kr = call(0x40, &s, sizeof(s))) != 0) {
		printf("mali_test: SUBMIT failed: %#x\n", kr);
		return 1;
	}
	memset(&wait, 0, sizeof(wait));
	wait.handles = (uint64_t)(uintptr_t)&syncobj;
	wait.count_handles = 1;
	wait.flags = 1;
	wait.timeout_nsec = INT64_MAX;
	kr = call(0xc3, &wait, sizeof(wait));
	uint64_t got;
	memcpy(&got, cpu + 0x8000, 8);
	printf("mali_test: job 1: wait %s, the GPU wrote %#llx (%s), exception status %#x\n", kr == 0 ? "ok" : "FAILED", got,
	    got == 0x0123456789abcdefULL ? "right" : "WRONG", *(uint32_t *)cpu);
	bad += kr != 0 || got != 0x0123456789abcdefULL;

	/* 2: many jobs, each its own value; the buffer waited for with WAIT_BO */
	for (unsigned i = 0; i < runs; i++) {
		uint64_t want = 0x5a5a000000000000ULL | i;
		struct wait_bo wb = { .handle = handle, .timeout_ns = INT64_MAX };

		write_value_job(cpu, va + 0x8000, want);
		t0 = mach_absolute_time();
		kr = call(0x40, &s, sizeof(s));
		t1 = mach_absolute_time();
		t_submit += t1 - t0;
		if (kr == 0) {
			kr = call(0x41, &wb, sizeof(wb));
		}
		t_wait += mach_absolute_time() - t1;
		memcpy(&got, cpu + 0x8000, 8);
		if (kr != 0 || got != want) {
			if (bad++ < 8) {
				printf("mali_test: job %u: %#x, wrote %#llx instead of %#llx\n", i + 2, kr, got, want);
			}
		}
	}
	memset(&c, 0, sizeof(c));
	call(0x7d, &c, sizeof(c));
	printf("mali_test: %u jobs: %.1f us to submit, %.1f us to done, each; the kernel counts %llu jobs, %llu faults, %llu resets\n",
	    runs, runs ? microseconds(t_submit) / runs : 0, runs ? microseconds(t_wait) / runs : 0, c.jobs, c.faults, c.resets);
	printf("mali_test: %s (%d bad)\n", bad == 0 ? "PASS" : "FAIL", bad);
	IOConnectUnmapMemory64(gpu, handle, mach_task_self(), map);
	IOServiceClose(gpu);
	return bad != 0;
}
