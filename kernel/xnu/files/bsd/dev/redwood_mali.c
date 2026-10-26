/*
 * The Galaxy S20+'s (y2s, Exynos 990) GPU, a Mali-G77 MP11 (Valhall, job manager): the hardware
 * half of its driver. iokit/Kernel/RedwoodMali.cpp gives it to programs with the interface of
 * Linux's panfrost; bsd/sys/redwood_mali.h is what is between the two.
 *
 * Boot args:
 *   mali=1          bring the GPU up and keep it (the service RedwoodMali appears)
 *   mali_selftest=1 ... and run a WRITE_VALUE job through the whole of this file at the start
 *   mali_trace=N    log the first N jobs
 *
 * What is done, and from where:
 *   power      Samsung's embedded_g3d_off then embedded_g3d_on (cal-if/exynos9830/
 *              flexpmu_cal_local_exynos9830.h), then the DTZPC restore their pmucal_local.c makes
 *              for a domain with need_smc: smc(0x82000410, 1, 0x18410204, 2). The bootloader leaves
 *              the domain on with PLL_G3D in its "off" setting, and in that state every memory
 *              access of the GPU's hangs: it must be cycled.
 *   init       as Linux's panfrost_gpu.c: soft reset, then the quirks of a G57 (the G77 is one to
 *              panfrost: Bentlybro/z3s-mainline-linux) -- SHADER_CONFIG SC_VAR_ALGORITHM (issue
 *              TTRX_2968/3162), JM_CONFIG IDVS group size 0xf -- then L2, shader cores, tiler on
 *   MMU        address space 0 in the AArch64 4 KB format (panfrost_mmu.c: mmu_cfg_init_aarch64_4k):
 *              tables written by this processor and cleaned out of its cache; memory write-back
 *              in the GPU's L2 (kbase on a GPU that is not coherent: memattr 0x8d, write-allocate,
 *              SH_MIDGARD_INNER) and inner shareable in the PTEs (kbase's KBASE_REG_SHARE_IN) so that
 *              the shader cores and the tiler see each other's writes within a job -- non-shareable,
 *              the tiler read vertex positions still in a core's cache: triangles across the screen
 *              (25 October). Kept right with the CPU by the flushes at each job's start and end;
 *              mali_cached=0: every access uncached instead; after a change, the region is locked and
 *              FLUSH_PT'd
 *   jobs       one at a time, in order, from a thread that looks at JOB_INT_RAWSTAT (no interrupts
 *              yet): fragment chains on slot 0, the rest on slot 1, both caches cleaned and
 *              invalidated at the start and end of each (panfrost_job_hw_submit()). A job that
 *              faults, an MMU fault, or 2 s without an answer: the GPU is reset and set up again,
 *              and the job counts as done (with a fault) so that nobody waits for it for ever.
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <mach/mach_types.h>
#include <os/atomic_private.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_mali.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern void redwood_mali_buffer_report(char *out, size_t length);   /* RedwoodMali.cpp */

/* mali_cached=0: every GPU access uncached, as before; 1 (the default): as kbase does on this SoC */
static uint32_t mali_cached = 1;

/* the kernel's message buffer keeps only a run's last half minute: bring-up facts go to the loader's log as well */
static void __printflike(1, 2)
mali_log(const char *fmt, ...)
{
	char line[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}
extern uint64_t kvtophys(vm_offset_t va);
extern uint64_t phystokv(uint64_t pa);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern void RedwoodMaliStart(void);

/* ------------------------------------------------------------------------ */
/* Power (Samsung's sequences, verified on the phone 24 Oct)                 */

#define PMU_PHYS                0x15860000U
#define PMU_G3D_CONFIGURATION   0x2200U
#define PMU_G3D_STATUS          0x2204U
#define PMU_G3D_OUT             0x2220U
#define PMU_ATOMIC_SET          0xc000U
#define CMU_TOP_PHYS            0x1a330000U
#define PLL_CON0_PLL_G3D        0x0100U
#define PLL_CON1_PLL_G3D        0x0104U
#define PLL_CON3_PLL_G3D        0x010cU  /* ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0; 26 MHz in */
#define CMU_G3D_PHYS            0x18400000U
#define MUX_G3D_USER            0x0600U
#define G3D_NEED_SMC            0x18410204U
#define SMC_PREPARE_PD_ONOFF    0x82000410U

#define MALI_PHYS               0x18500000U
#define MALI_SIZE               0x5000U

/* ------------------------------------------------------------------------ */
/* Registers (Linux drivers/gpu/drm/panfrost/panfrost_regs.h)                */

#define GPU_ID                  0x000U
#define GPU_L2_FEATURES         0x004U
#define GPU_CORE_FEATURES       0x008U
#define GPU_TILER_FEATURES      0x00cU
#define GPU_MEM_FEATURES        0x010U
#define GPU_MMU_FEATURES        0x014U
#define GPU_AS_PRESENT          0x018U
#define GPU_JS_PRESENT          0x01cU
#define GPU_INT_RAWSTAT         0x020U
#define GPU_INT_CLEAR           0x024U
#define GPU_INT_MASK            0x028U
#define GPU_CMD                 0x030U
#define GPU_STATUS              0x034U
#define GPU_FAULT_STATUS        0x03cU
#define GPU_FAULT_ADDRESS_LO    0x040U
#define GPU_FAULT_ADDRESS_HI    0x044U
#define GPU_AFBC_FEATURES       0x04cU
#define GPU_CYCLE_COUNT_LO      0x090U
#define GPU_CYCLE_COUNT_HI      0x094U
#define GPU_TIMESTAMP_LO        0x098U
#define GPU_TIMESTAMP_HI        0x09cU
#define GPU_THREAD_MAX_THREADS  0x0a0U
#define GPU_THREAD_MAX_WG_SIZE  0x0a4U
#define GPU_THREAD_MAX_BARRIER  0x0a8U
#define GPU_THREAD_FEATURES     0x0acU
#define GPU_TEXTURE_FEATURES(n) (0x0b0U + 4 * (n))
#define GPU_JS_FEATURES(n)      (0x0c0U + 4 * (n))
#define GPU_SHADER_PRESENT_LO   0x100U
#define GPU_SHADER_PRESENT_HI   0x104U
#define GPU_TILER_PRESENT_LO    0x110U
#define GPU_L2_PRESENT_LO       0x120U
#define GPU_COHERENCY_FEATURES  0x300U
#define GPU_THREAD_TLS_ALLOC    0x310U
#define GPU_STACK_PRESENT_LO    0xe00U
#define GPU_JM_CONFIG           0xf00U
#define GPU_SHADER_CONFIG       0xf04U
#define SHADER_READY_LO         0x140U
#define TILER_READY_LO          0x150U
#define L2_READY_LO             0x160U
#define SHADER_PWRON_LO         0x180U
#define TILER_PWRON_LO          0x190U
#define L2_PWRON_LO             0x1a0U

#define GPU_CMD_SOFT_RESET      0x01U
#define GPU_CMD_HARD_RESET      0x02U
#define GPU_CMD_CYCLE_COUNT_START 0x05U  /* the GPU counts its own clock: the MHz it really runs at */
#define GPU_IRQ_RESET_COMPLETED (1U << 8)

#define SC_VAR_ALGORITHM        (1U << 29)
#define JM_IDVS_GROUP_SIZE(n)   ((n) << 16)

#define JOB_INT_RAWSTAT         0x1000U
#define JOB_INT_CLEAR           0x1004U
#define JOB_INT_MASK            0x1008U
#define JS(n, r)                (0x1800U + (n) * 0x80U + (r))
#define JS_HEAD_LO              0x00U
#define JS_HEAD_HI              0x04U
#define JS_COMMAND              0x20U
#define JS_STATUS               0x24U
#define JS_HEAD_NEXT_LO         0x40U
#define JS_HEAD_NEXT_HI         0x44U
#define JS_AFFINITY_NEXT_LO     0x50U
#define JS_AFFINITY_NEXT_HI     0x54U
#define JS_CONFIG_NEXT          0x58U
#define JS_COMMAND_NEXT         0x60U
#define JS_COMMAND_START        0x01U
#define JS_COMMAND_HARD_STOP    0x03U
#define JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE (3U << 8)
#define JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE   (3U << 12)
#define JS_CONFIG_THREAD_PRI(n) ((n) << 16)

#define MMU_INT_RAWSTAT         0x2000U
#define MMU_INT_CLEAR           0x2004U
#define MMU_INT_MASK            0x2008U
#define AS(n, r)                (0x2400U + ((n) << 6) + (r))
#define AS_TRANSTAB_LO          0x00U
#define AS_TRANSTAB_HI          0x04U
#define AS_MEMATTR_LO           0x08U
#define AS_MEMATTR_HI           0x0cU
#define AS_LOCKADDR_LO          0x10U
#define AS_LOCKADDR_HI          0x14U
#define AS_COMMAND              0x18U
#define AS_FAULTSTATUS          0x1cU
#define AS_FAULTADDRESS_LO      0x20U
#define AS_FAULTADDRESS_HI      0x24U
#define AS_STATUS               0x28U
#define AS_TRANSCFG_LO          0x30U
#define AS_TRANSCFG_HI          0x34U
#define AS_COMMAND_UPDATE       0x01U
#define AS_COMMAND_LOCK         0x02U
#define AS_COMMAND_FLUSH_PT     0x04U
#define AS_COMMAND_FLUSH_MEM    0x05U
#define AS_STATUS_ACTIVE        0x01U
#define AS_TRANSCFG_ADRMODE_AARCH64_4K  6U
#define AS_TRANSCFG_INA_BITS(x) ((x) << 6)
#define AS_TRANSCFG_PTW_MEMATTR_WB      (2U << 24)
#define AS_TRANSCFG_PTW_RA      (1U << 30)
#define MEMATTR_NC              ((1U << 6) | (0U << 4) | (3U << 2))
#define MEMATTR_WB_WRITE_ALLOC  0x8dU   /* kbase's AS_MEMATTR_WRITE_ALLOC: write-back in the GPU's L2 */

/* ARM LPAE stage-1 descriptors (io-pgtable-arm.c, as panfrost has them) */
#define PTE_TABLE               3ULL
#define PTE_PAGE                3ULL
#define PTE_AP_UNPRIV           (1ULL << 6)
#define PTE_AP_RDONLY           (2ULL << 6)
#define PTE_SH_OS               (2ULL << 8)
#define PTE_SH_IS               (3ULL << 8)    /* with memattr SH_MIDGARD_INNER: coherent between the GPU's own cores */
#define PTE_AF                  (1ULL << 10)
#define PTE_NG                  (1ULL << 11)
#define PTE_XN                  (1ULL << 54)
#define PTE_ADDR(e)             ((e) & 0x0000fffffffff000ULL)

#define VA_START                0x02000000ULL   /* 32 MB .. 4 GB, as Linux's panfrost */
#define VA_END                  0x100000000ULL
#define VA_EXTENTS              4096

#define JOBS                    256
#define JOB_TIMEOUT_MS          2000
#define POLL_US                 20

/* ------------------------------------------------------------------------ */

static volatile uint8_t *pmu, *cmu_top, *cmu_g3d, *mali;
static LCK_GRP_DECLARE(mali_grp, "redwood-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_grp);       /* everything below */
static bool mali_up;
static uint32_t trace;

static struct {
	uint32_t gpu_id, l2_features, core_features, tiler_features, mem_features, mmu_features;
	uint32_t as_present, js_present, thread_max, thread_wg, thread_barrier, thread_features, tls_alloc;
	uint32_t texture[4], js[16], afbc, coherency;
	uint32_t shader_lo, shader_hi, tiler, l2, stack;
} hw;

static inline uint32_t
rd(volatile uint8_t *base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(volatile uint8_t *base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static inline uint32_t
mr(uint32_t off)
{
	return rd(mali, off);
}

static inline void
mw(uint32_t off, uint32_t value)
{
	wr(mali, off, value);
}

static bool
poll_reg(uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i < us / 5; i++) {
		if ((mr(off) & mask) == want) {
			return true;
		}
		delay(5);
	}
	return (mr(off) & mask) == want;
}

static uint64_t
smc4(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static void
clean_lines(const void *start, uint64_t size, bool invalidate)
{
	uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)start + size;

	for (; a < end; a += 64) {
		if (invalidate) {
			__asm__ volatile ("dc civac, %0" : : "r"(a) : "memory");
		} else {
			__asm__ volatile ("dc cvac, %0" : : "r"(a) : "memory");
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
redwood_mali_to_memory(const void *start, uint64_t size)
{
	if (start == NULL) {
		__asm__ volatile ("dsb sy" ::: "memory");
		return;
	}
	clean_lines(start, size, true);
}

static bool
g3d_on(void)
{
	return (rd(pmu, PMU_G3D_STATUS) & 1) != 0;
}

/* embedded_g3d_off[], embedded_g3d_on[], the DTZPC restore */
static bool
power_cycle(void)
{
	uint32_t i;
	uint64_t ret;

	if (g3d_on()) {
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~1U);
		for (i = 0; i < 1000 && g3d_on(); i++) {
			delay(10);
		}
		if (g3d_on()) {
			printf("mali: G3D does not power off\n");
			return false;
		}
		wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) & ~(1U << 4));
		wr(cmu_top, PLL_CON0_PLL_G3D, rd(cmu_top, PLL_CON0_PLL_G3D) & ~(1U << 4));
		wr(cmu_top, PLL_CON1_PLL_G3D, 0x10000001);
	}
	wr(cmu_top, PLL_CON1_PLL_G3D, 0x30000003);
	wr(cmu_top, PLL_CON0_PLL_G3D, rd(cmu_top, PLL_CON0_PLL_G3D) | (1U << 4));
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) | (1U << 4));
	wr(pmu, PMU_G3D_OUT | PMU_ATOMIC_SET, 0x2);
	delay(0x118);
	wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) | 1);
	for (i = 0; i < 1000 && !g3d_on(); i++) {
		delay(10);
	}
	if (!g3d_on()) {
		printf("mali: G3D does not power on\n");
		return false;
	}
	ret = smc4(SMC_PREPARE_PD_ONOFF, 1, G3D_NEED_SMC, 2);
	{
		uint32_t con3 = rd(cmu_top, PLL_CON3_PLL_G3D), m = (con3 >> 16) & 0x3ff, p = (con3 >> 8) & 0x3f, s = con3 & 7;

		mali_log("mali: G3D cycled: PLL_G3D %#x %#x %#x (M %u P %u S %u: %u MHz), mux %#x, DTZPC restore answers %#llx\n",
		    rd(cmu_top, PLL_CON0_PLL_G3D), rd(cmu_top, PLL_CON1_PLL_G3D), con3, m, p, s, p ? 26 * m / (p << s) : 0,
		    rd(cmu_g3d, MUX_G3D_USER), ret);
	}
	return true;
}

static void
read_features(void)
{
	hw.gpu_id = mr(GPU_ID);
	hw.l2_features = mr(GPU_L2_FEATURES);
	hw.core_features = mr(GPU_CORE_FEATURES);
	hw.tiler_features = mr(GPU_TILER_FEATURES);
	hw.mem_features = mr(GPU_MEM_FEATURES);
	hw.mmu_features = mr(GPU_MMU_FEATURES);
	hw.as_present = mr(GPU_AS_PRESENT);
	hw.js_present = mr(GPU_JS_PRESENT);
	hw.thread_max = mr(GPU_THREAD_MAX_THREADS);
	hw.thread_wg = mr(GPU_THREAD_MAX_WG_SIZE);
	hw.thread_barrier = mr(GPU_THREAD_MAX_BARRIER);
	hw.thread_features = mr(GPU_THREAD_FEATURES);
	hw.tls_alloc = mr(GPU_THREAD_TLS_ALLOC);
	for (unsigned i = 0; i < 4; i++) {
		hw.texture[i] = mr(GPU_TEXTURE_FEATURES(i));
	}
	for (unsigned i = 0; i < 16; i++) {
		hw.js[i] = mr(GPU_JS_FEATURES(i));
	}
	hw.afbc = mr(GPU_AFBC_FEATURES);
	hw.coherency = mr(GPU_COHERENCY_FEATURES);
	hw.shader_lo = mr(GPU_SHADER_PRESENT_LO);
	hw.shader_hi = mr(GPU_SHADER_PRESENT_HI);
	hw.tiler = mr(GPU_TILER_PRESENT_LO);
	hw.l2 = mr(GPU_L2_PRESENT_LO);
	hw.stack = mr(GPU_STACK_PRESENT_LO);
}


/* ------------------------------------------------------------------------ */
/* Page tables                                                              */

/*
 * Four levels of 512 entries. Each table has a node of the kernel's: the table, and for levels
 * 0-2 a page of 512 pointers to the nodes below (the GPU's table holds physical addresses only).
 * 4 GB of addresses take at most 1 + 1 + 4 + 2048 tables. Tables come four to a 16 KB page of
 * the kernel's and are never given back.
 */
#define PT_NODES                2100

struct pt_node {
	uint64_t        *table;
	struct pt_node  **child;
};

static struct pt_node pt_nodes[PT_NODES];
static unsigned pt_nodes_used;
static struct pt_node *pt_root;
static vm_offset_t pool;
static unsigned pool_left;

static void *
table_new(void)
{
	if (pool_left == 0) {
		if (kmem_alloc(kernel_map, &pool, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return NULL;
		}
		pool_left = PAGE_SIZE / 4096;
	}
	return (void *)(pool + PAGE_SIZE - 4096 * pool_left--);
}

static struct pt_node *
node_new(bool leaf)
{
	struct pt_node *n;

	if (pt_nodes_used == PT_NODES) {
		return NULL;
	}
	n = &pt_nodes[pt_nodes_used];
	n->table = table_new();
	n->child = leaf ? NULL : table_new();
	if (n->table == NULL || (!leaf && n->child == NULL)) {
		return NULL;
	}
	pt_nodes_used++;
	clean_lines(n->table, 4096, false);     /* zeroes, for the GPU's table walk */
	return n;
}

/* The level-3 table for `va`, made with those above it if `make`. NULL: none, or no memory. */
static uint64_t *
l3_table(uint64_t va, bool make)
{
	struct pt_node *n = pt_root;

	for (int level = 0; level < 3; level++) {
		unsigned i = (unsigned)(va >> (39 - 9 * level)) & 511;

		if (n->child[i] == NULL) {
			struct pt_node *c;

			if (!make || (c = node_new(level == 2)) == NULL) {
				return NULL;
			}
			n->table[i] = kvtophys((vm_offset_t)c->table) | PTE_TABLE;
			clean_lines(&n->table[i], 8, false);
			n->child[i] = c;
		}
		n = n->child[i];
	}
	return n->table;
}

/* ------------------------------------------------------------------------ */
/* Address space 0                                                           */

static bool
as_wait(void)
{
	if (!poll_reg(AS(0, AS_STATUS), AS_STATUS_ACTIVE, 0, 100000)) {
		printf("mali: AS0 stays busy (status %#x, fault %#x)\n", mr(AS(0, AS_STATUS)), mr(AS(0, AS_FAULTSTATUS)));
		return false;
	}
	return true;
}

/* panfrost's lock_region() + the command: a naturally aligned power of two around [va, va + size) */
static bool
as_operation(uint64_t va, uint64_t size, uint32_t command)
{
	uint64_t end = va + size, region;
	unsigned width = 64 - (unsigned)__builtin_clzll(va ^ (end - 1));

	if (size == 0) {
		return true;
	}
	if (width < 15) {
		width = 15;                     /* AS_LOCK_REGION_MIN_SIZE: 32 KB */
	}
	width -= 1;
	region = (va & ~((1ULL << width) - 1)) | width;
	if (!as_wait()) {
		return false;
	}
	mw(AS(0, AS_LOCKADDR_LO), (uint32_t)region);
	mw(AS(0, AS_LOCKADDR_HI), (uint32_t)(region >> 32));
	mw(AS(0, AS_COMMAND), AS_COMMAND_LOCK);
	if (!as_wait()) {
		return false;
	}
	mw(AS(0, AS_COMMAND), command);
	return as_wait();
}

static bool
as_enable(void)
{
	uint64_t transtab = kvtophys((vm_offset_t)pt_root->table);
	uint64_t transcfg = AS_TRANSCFG_ADRMODE_AARCH64_4K | AS_TRANSCFG_INA_BITS(55 - 48) | AS_TRANSCFG_PTW_MEMATTR_WB |
	    AS_TRANSCFG_PTW_RA;

	if (!as_operation(0, ~0ULL, AS_COMMAND_FLUSH_MEM)) {
		return false;
	}
	mw(AS(0, AS_TRANSTAB_LO), (uint32_t)transtab);
	mw(AS(0, AS_TRANSTAB_HI), (uint32_t)(transtab >> 32));
	uint32_t memattr = mali_cached ? MEMATTR_WB_WRITE_ALLOC : MEMATTR_NC;

	mw(AS(0, AS_MEMATTR_LO), memattr * 0x01010101U);
	mw(AS(0, AS_MEMATTR_HI), memattr * 0x01010101U);
	mw(AS(0, AS_TRANSCFG_LO), (uint32_t)transcfg);
	mw(AS(0, AS_TRANSCFG_HI), (uint32_t)(transcfg >> 32));
	mw(AS(0, AS_COMMAND), AS_COMMAND_UPDATE);
	return as_wait();
}

bool
redwood_mali_map(uint64_t va, uint64_t pa, uint32_t flags)
{
	uint64_t *l3, entry;
	bool ok = false;

	if ((va & 4095) || (pa & 4095) || va < VA_START || va >= VA_END) {
		return false;
	}
	entry = pa | PTE_PAGE | PTE_AF | PTE_NG | PTE_AP_UNPRIV | (mali_cached ? PTE_SH_IS : PTE_SH_OS) |
	    ((flags & REDWOOD_MALI_MAP_READ_ONLY) ? PTE_AP_RDONLY : 0) | ((flags & REDWOOD_MALI_MAP_NOEXEC) ? PTE_XN : 0);
	lck_mtx_lock(&mali_lock);
	if (mali_up && (l3 = l3_table(va, true)) != NULL) {
		unsigned i = (unsigned)(va >> 12) & 511;

		l3[i] = entry;
		__asm__ volatile ("dc cvac, %0" : : "r"(&l3[i]) : "memory");
		ok = true;
	}
	lck_mtx_unlock(&mali_lock);
	return ok;
}

void
redwood_mali_mapped(uint64_t va, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	__asm__ volatile ("dsb sy" ::: "memory");
	if (mali_up) {
		as_operation(va, size, AS_COMMAND_FLUSH_PT);
	}
	lck_mtx_unlock(&mali_lock);
}

void
redwood_mali_unmap(uint64_t va, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	if (mali_up) {
		for (uint64_t a = va; a < va + size; a += 4096) {
			uint64_t *l3 = l3_table(a, false);

			if (l3 != NULL) {
				l3[(a >> 12) & 511] = 0;
				__asm__ volatile ("dc cvac, %0" : : "r"(&l3[(a >> 12) & 511]) : "memory");
			}
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		as_operation(va, size, AS_COMMAND_FLUSH_PT);
	}
	lck_mtx_unlock(&mali_lock);
}

/* ------------------------------------------------------------------------ */
/* GPU addresses: a sorted list of what is free                             */

static struct {
	uint64_t start, size;
} va_free_list[VA_EXTENTS];
static unsigned va_free_count;

uint64_t
redwood_mali_va_alloc(uint64_t size, bool exec)
{
	uint64_t align = 4096, got = 0;

	size = (size + 4095) & ~4095ULL;
	if (size == 0) {
		return 0;
	}
	if (exec) {
		/* a power of two at least the size: then it cannot cross a 16 MB line */
		while (align < size && align < (16ULL << 20)) {
			align <<= 1;
		}
	} else if (size >= (2ULL << 20)) {
		align = 2ULL << 20;
	}
	lck_mtx_lock(&mali_lock);
	for (unsigned i = 0; i < va_free_count; i++) {
		uint64_t s = va_free_list[i].start, e = s + va_free_list[i].size;
		uint64_t a = (s + align - 1) & ~(align - 1);

		if (a + size > e) {
			continue;
		}
		got = a;
		if (a == s && a + size == e) {
			memmove(&va_free_list[i], &va_free_list[i + 1], (va_free_count - i - 1) * sizeof(va_free_list[0]));
			va_free_count--;
		} else if (a == s) {
			va_free_list[i].start += size;
			va_free_list[i].size -= size;
		} else if (a + size == e) {
			va_free_list[i].size = a - s;
		} else if (va_free_count < VA_EXTENTS) {
			memmove(&va_free_list[i + 2], &va_free_list[i + 1], (va_free_count - i - 1) * sizeof(va_free_list[0]));
			va_free_count++;
			va_free_list[i].size = a - s;
			va_free_list[i + 1].start = a + size;
			va_free_list[i + 1].size = e - (a + size);
		} else {
			got = 0;
			continue;
		}
		break;
	}
	lck_mtx_unlock(&mali_lock);
	return got;
}

void
redwood_mali_va_free(uint64_t va, uint64_t size)
{
	unsigned i;

	size = (size + 4095) & ~4095ULL;
	if (va == 0 || size == 0) {
		return;
	}
	lck_mtx_lock(&mali_lock);
	for (i = 0; i < va_free_count && va_free_list[i].start < va; i++) {
	}
	/* join the one before, the one after, or both; else a new one */
	bool before = i > 0 && va_free_list[i - 1].start + va_free_list[i - 1].size == va;
	bool after = i < va_free_count && va + size == va_free_list[i].start;

	if (before && after) {
		va_free_list[i - 1].size += size + va_free_list[i].size;
		memmove(&va_free_list[i], &va_free_list[i + 1], (va_free_count - i - 1) * sizeof(va_free_list[0]));
		va_free_count--;
	} else if (before) {
		va_free_list[i - 1].size += size;
	} else if (after) {
		va_free_list[i].start = va;
		va_free_list[i].size += size;
	} else if (va_free_count < VA_EXTENTS) {
		memmove(&va_free_list[i + 1], &va_free_list[i], (va_free_count - i) * sizeof(va_free_list[0]));
		va_free_list[i].start = va;
		va_free_list[i].size = size;
		va_free_count++;
	} else {
		printf("mali: the list of free GPU addresses is full: %#llx..+%#llx lost\n", va, size);
	}
	lck_mtx_unlock(&mali_lock);
}

/* ------------------------------------------------------------------------ */
/* Bring-up and reset                                                       */

/* With the lock held: from a soft reset to a GPU ready for jobs in address space 0. */
static bool
hw_init(const char *why)
{
	mw(GPU_INT_MASK, 0);
	mw(GPU_INT_CLEAR, GPU_IRQ_RESET_COMPLETED);
	mw(GPU_CMD, GPU_CMD_SOFT_RESET);
	if (!poll_reg(GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100000)) {
		printf("mali: %s: soft reset did not complete, hard reset\n", why);
		mw(GPU_CMD, GPU_CMD_HARD_RESET);
		if (!poll_reg(GPU_INT_RAWSTAT, GPU_IRQ_RESET_COMPLETED, GPU_IRQ_RESET_COMPLETED, 100000)) {
			printf("mali: %s: hard reset did not complete either\n", why);
			return false;
		}
	}
	mw(GPU_INT_CLEAR, 0xffffffff);
	mw(JOB_INT_MASK, 0);
	mw(JOB_INT_CLEAR, 0xffffffff);
	mw(MMU_INT_MASK, 0);
	mw(MMU_INT_CLEAR, 0xffffffff);

	/* panfrost_gpu_init_quirks() for a G57 */
	mw(GPU_SHADER_CONFIG, SC_VAR_ALGORITHM);
	mw(GPU_JM_CONFIG, JM_IDVS_GROUP_SIZE(0xf));

	mw(L2_PWRON_LO, hw.l2);
	if (!poll_reg(L2_READY_LO, 0xffffffff, hw.l2, 20000)) {
		printf("mali: %s: L2 not ready (%#x)\n", why, mr(L2_READY_LO));
		return false;
	}
	mw(SHADER_PWRON_LO, hw.shader_lo);
	mw(TILER_PWRON_LO, hw.tiler);
	if (!poll_reg(SHADER_READY_LO, 0xffffffff, hw.shader_lo, 20000) || !poll_reg(TILER_READY_LO, 0xffffffff, hw.tiler, 20000)) {
		printf("mali: %s: cores not ready (shaders %#x of %#x, tiler %#x)\n", why, mr(SHADER_READY_LO), hw.shader_lo,
		    mr(TILER_READY_LO));
		return false;
	}
	mw(GPU_CMD, GPU_CMD_CYCLE_COUNT_START);
	return as_enable();
}

/* ------------------------------------------------------------------------ */
/* Jobs                                                                     */

static struct job {
	uint64_t        jc, seqno;
	uint32_t        requirements;
	int             pid;                    /* who submitted it, for saying whose job failed */
	char            name[20];
} jobs[JOBS];
static unsigned job_head, job_tail;     /* taken from head, added at tail */
static uint64_t submitted, completed;
static uint64_t stat_jobs, stat_faults, stat_resets;

uint64_t
redwood_mali_submit(uint64_t jc, uint32_t requirements)
{
	uint64_t seqno = 0;

	lck_mtx_lock(&mali_lock);
	while (mali_up && job_tail - job_head == JOBS) {
		lck_mtx_sleep(&mali_lock, LCK_SLEEP_DEFAULT, (event_t)&completed, THREAD_UNINT);
	}
	if (mali_up) {
		struct job *j = &jobs[job_tail++ % JOBS];

		j->jc = jc;
		j->requirements = requirements;
		j->pid = proc_selfpid();
		proc_selfname(j->name, sizeof(j->name));
		j->seqno = seqno = ++submitted;
		thread_wakeup((event_t)&jobs);
	}
	lck_mtx_unlock(&mali_lock);
	return seqno;
}

bool
redwood_mali_done(uint64_t seqno)
{
	return os_atomic_load(&completed, acquire) >= seqno;
}

uint64_t
redwood_mali_last(void)
{
	return os_atomic_load(&submitted, relaxed);
}

bool
redwood_mali_wait(uint64_t seqno, uint64_t milliseconds)
{
	uint64_t deadline;
	bool done;

	if (redwood_mali_done(seqno)) {
		return true;
	}
	if (milliseconds == 0) {
		return false;
	}
	clock_interval_to_deadline((uint32_t)milliseconds, NSEC_PER_MSEC, &deadline);
	lck_mtx_lock(&mali_lock);
	while (!(done = completed >= seqno) && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_lock, LCK_SLEEP_DEFAULT, (event_t)&completed, THREAD_UNINT, deadline);
	}
	lck_mtx_unlock(&mali_lock);
	return done;
}

void
redwood_mali_counts(uint64_t *j, uint64_t *f, uint64_t *r)
{
	*j = stat_jobs;
	*f = stat_faults;
	*r = stat_resets;
}

static void
job_start(const struct job *j)
{
	unsigned slot = (j->requirements & REDWOOD_MALI_JOB_FS) ? 0 : 1;

	mw(JS(slot, JS_HEAD_NEXT_LO), (uint32_t)j->jc);
	mw(JS(slot, JS_HEAD_NEXT_HI), (uint32_t)(j->jc >> 32));
	mw(JS(slot, JS_AFFINITY_NEXT_LO), hw.shader_lo);
	mw(JS(slot, JS_AFFINITY_NEXT_HI), hw.shader_hi);
	mw(JS(slot, JS_CONFIG_NEXT), 0 /* AS 0 */ | JS_CONFIG_THREAD_PRI(8) | JS_CONFIG_START_FLUSH_CLEAN_INVALIDATE |
	    JS_CONFIG_END_FLUSH_CLEAN_INVALIDATE);
	mw(JS(slot, JS_COMMAND_NEXT), JS_COMMAND_START);
}

static uint64_t last_fault_jc;                  /* the chain of the last job that failed (the counts' fourth word) */

uint64_t
redwood_mali_last_fault(void)
{
	return os_atomic_load(&last_fault_jc, relaxed);
}

/* A GPU address read through the page table, for saying what a failed job was given; 0 when unmapped. */
static bool
gpu_read64(uint64_t va, uint64_t *value)
{
	uint64_t *l3 = l3_table(va & ~4095ULL, false), entry;
	volatile uint64_t *p;

	if (l3 == NULL || ((entry = l3[(va >> 12) & 511]) & 3) != PTE_PAGE) {
		return false;
	}
	p = (volatile uint64_t *)phystokv(PTE_ADDR(entry) | (va & 4088));
	__asm__ volatile ("dc civac, %0\n dsb sy" : : "r"(p) : "memory");
	*value = *p;
	return true;
}

/* `words` 64-bit words from `va`, eight to a line, into the run's log under `what` */
static void
gpu_dump(const char *what, uint64_t va, unsigned words)
{
	char line[256];

	for (unsigned w = 0; w < words; w += 8) {
		size_t at = (size_t)snprintf(line, sizeof(line), "mali:   %s +%#x:", what, w * 8);

		for (unsigned k = 0; k < 8 && w + k < words; k++) {
			uint64_t v = 0;

			if (gpu_read64(va + 8 * (w + k), &v)) {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " %llx", v);
			} else {
				at += (size_t)snprintf(line + at, sizeof(line) - at, " --");
			}
		}
		snprintf(line + at, sizeof(line) - at, "\n");
		loader_log_line(line);
	}
}

/* With the lock held: what went wrong, then a reset and the GPU set up again. */
static void
job_failed(const struct job *j, const char *what)
{
	unsigned slot = (j->requirements & REDWOOD_MALI_JOB_FS) ? 0 : 1;

	stat_faults++;
	last_fault_jc = j->jc;
	if (stat_faults <= 32) {
		char line[320];

		snprintf(line, sizeof(line), "mali: job %llu of %s[%d] (chain %#llx, slot %u) %s: JS status %#x head %#x:%08x; job rawstat %#x; "
		    "MMU rawstat %#x, AS0 fault %#x at %#x:%08x; GPU fault %#x at %#x:%08x\n", j->seqno, j->name, j->pid, j->jc, slot, what,
		    mr(JS(slot, JS_STATUS)), mr(JS(slot, JS_HEAD_HI)), mr(JS(slot, JS_HEAD_LO)), mr(JOB_INT_RAWSTAT), mr(MMU_INT_RAWSTAT),
		    mr(AS(0, AS_FAULTSTATUS)), mr(AS(0, AS_FAULTADDRESS_HI)), mr(AS(0, AS_FAULTADDRESS_LO)), mr(GPU_FAULT_STATUS),
		    mr(GPU_FAULT_ADDRESS_HI), mr(GPU_FAULT_ADDRESS_LO));
		printf("%s", line);
		loader_log_line(line);  /* (the kernel's own message buffer keeps only a run's last half minute) */
		if (slot == 0 && stat_faults <= 3) {
			/* a fragment job: its header and payload, then the framebuffer descriptor it points at */
			uint64_t fbd = 0;

			gpu_dump("job", j->jc, 8);
			if (gpu_read64(j->jc + 0x28, &fbd)) {
				gpu_dump("fbd", fbd & ~0x3fULL, 64);
			}
		}
	}
	mw(JS(slot, JS_COMMAND), JS_COMMAND_HARD_STOP);
	delay(100);
	stat_resets++;
	if (!hw_init("after a fault")) {
		printf("mali: the GPU could not be set up again: it is given nothing more\n");
		mali_up = false;
	}
}

/*
 * mali_irq=1: the job interrupt (dt: mali@18500000 "JOB", SPI 0x12d, level) wakes the scheduler.
 * Masked at the GPU here, in interrupt context, so the level line drops until the scheduler has
 * looked; the scheduler arms it again for the next job.
 */
static bool use_irq;
static int job_irq_event;
static uint64_t stat_irqs;

#define MALI_JOB_INTID          (0x12d + 32)

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context);

static void
job_irq(__unused void *context, __unused uint32_t intid)
{
	mw(JOB_INT_MASK, 0);
	stat_irqs++;
	thread_wakeup((event_t)&job_irq_event);
}

/*
 * Where the GPU's time goes, every 5 s (printf, so in the log's message buffer): jobs by kind, how
 * long they ran from start to done, the longest, and how long the GPU sat with nothing to run.
 */
static struct {
	uint64_t window_start, busy[2], longest[2], idle, idle_since, cycles;
	uint32_t count[2];
} gpu_time;

static uint64_t
gpu_cycles(void)
{
	uint32_t hi, lo;

	do {
		hi = mr(GPU_CYCLE_COUNT_HI);
		lo = mr(GPU_CYCLE_COUNT_LO);
	} while (hi != mr(GPU_CYCLE_COUNT_HI));
	return ((uint64_t)hi << 32) | lo;
}

static void
gpu_time_done(unsigned slot, uint64_t ran, uint64_t now)
{
	uint64_t ns;

	gpu_time.count[slot]++;
	gpu_time.busy[slot] += ran;
	gpu_time.longest[slot] = ran > gpu_time.longest[slot] ? ran : gpu_time.longest[slot];
	gpu_time.idle_since = now;
	absolutetime_to_nanoseconds(now - gpu_time.window_start, &ns);
	if (gpu_time.window_start == 0) {
		gpu_time.window_start = now;
		gpu_time.cycles = gpu_cycles();
	} else if (ns >= 5ULL * NSEC_PER_SEC) {
		uint64_t b0, b1, l0, l1, idle, cycles = gpu_cycles();
		char line[600];
		int used;

		absolutetime_to_nanoseconds(gpu_time.busy[0], &b0);
		absolutetime_to_nanoseconds(gpu_time.busy[1], &b1);
		absolutetime_to_nanoseconds(gpu_time.longest[0], &l0);
		absolutetime_to_nanoseconds(gpu_time.longest[1], &l1);
		absolutetime_to_nanoseconds(gpu_time.idle, &idle);
		used = snprintf(line, sizeof(line), "mali: %llu ms: %u fragment jobs %llu ms (longest %llu.%llu), %u vertex/tiler/compute "
		    "jobs %llu ms (longest %llu.%llu); GPU idle %llu ms%s; GPU clock %llu MHz (its cycle counter); ",
		    ns / NSEC_PER_MSEC, gpu_time.count[0], b0 / NSEC_PER_MSEC, l0 / NSEC_PER_MSEC, (l0 / 100000) % 10,
		    gpu_time.count[1], b1 / NSEC_PER_MSEC, l1 / NSEC_PER_MSEC, (l1 / 100000) % 10, idle / NSEC_PER_MSEC,
		    use_irq ? " (job interrupt)" : " (polled)", ns ? (cycles - gpu_time.cycles) * 1000 / ns : 0);
		if (used > 0 && (size_t)used < sizeof(line) - 2) {
			redwood_mali_buffer_report(line + used, sizeof(line) - 2 - used);
		}
		strlcat(line, "\n", sizeof(line));
		loader_log_line(line);
		memset(&gpu_time, 0, sizeof(gpu_time));
		gpu_time.window_start = now;
		gpu_time.idle_since = now;
		gpu_time.cycles = cycles;
	}
}

static void
scheduler(__unused void *parameter, __unused wait_result_t result)
{
	struct job *running = NULL;
	uint64_t started = 0, timeout;

	nanoseconds_to_absolutetime((uint64_t)JOB_TIMEOUT_MS * NSEC_PER_MSEC, &timeout);
	thread_set_thread_name(current_thread(), "mali scheduler");
	lck_mtx_lock(&mali_lock);
	for (;;) {
		if (running == NULL) {
			if (job_head == job_tail || !mali_up) {
				lck_mtx_sleep(&mali_lock, LCK_SLEEP_DEFAULT, (event_t)&jobs, THREAD_UNINT);
				continue;
			}
			running = &jobs[job_head % JOBS];
			__asm__ volatile ("dsb sy" ::: "memory");
			job_start(running);
			started = mach_absolute_time();
			if (gpu_time.idle_since != 0 && started > gpu_time.idle_since) {
				gpu_time.idle += started - gpu_time.idle_since;
			}
			if (stat_jobs < trace) {
				printf("mali: job %llu: chain %#llx, %s\n", running->seqno, running->jc,
				    (running->requirements & REDWOOD_MALI_JOB_FS) ? "fragment" : "vertex/tiler/compute");
			}
		}
		unsigned slot = (running->requirements & REDWOOD_MALI_JOB_FS) ? 0 : 1;

		lck_mtx_unlock(&mali_lock);
		if (use_irq) {
			/*
			 * mali_irq=1: asleep until the job interrupt (SPI 0x12d) says this slot is done or
			 * failed, instead of looking every POLL_US -- which kept one of the six slow cores
			 * busy for as long as the GPU drew. Armed after assert_wait, so an interrupt that
			 * comes before thread_block is not lost; a re-check every 2 ms catches anything
			 * the interrupt does not say (an MMU fault, a timeout).
			 */
			uint32_t bits = (1U << slot) | (1U << (16 + slot));
			uint64_t deadline;

			clock_interval_to_deadline(2, NSEC_PER_MSEC, &deadline);
			assert_wait_deadline((event_t)&job_irq_event, THREAD_UNINT, deadline);
			mw(JOB_INT_MASK, bits);
			if ((mr(JOB_INT_RAWSTAT) & bits) != 0 || mr(MMU_INT_RAWSTAT) != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			mw(JOB_INT_MASK, 0);
		} else {
			delay(POLL_US);
		}
		lck_mtx_lock(&mali_lock);

		uint32_t raw = mr(JOB_INT_RAWSTAT), mmu = mr(MMU_INT_RAWSTAT);

		if (raw & (1U << (16 + slot))) {
			job_failed(running, "faulted");
		} else if (mmu != 0) {
			job_failed(running, "made an MMU fault");
		} else if (raw & (1U << slot)) {
			mw(JOB_INT_CLEAR, 1U << slot);
		} else if (mach_absolute_time() - started > timeout) {
			job_failed(running, "did not finish in time");
		} else {
			continue;
		}
		gpu_time_done(slot, mach_absolute_time() - started, mach_absolute_time());
		stat_jobs++;
		if (use_irq && (stat_jobs == 1000 || stat_jobs == 100000)) {
			mali_log("mali: %llu jobs, %llu job interrupts taken\n", stat_jobs, stat_irqs);
		}
		job_head++;
		os_atomic_store(&completed, running->seqno, release);
		running = NULL;
		thread_wakeup((event_t)&completed);
	}
}

/* ------------------------------------------------------------------------ */
/* Parameters (Linux's DRM_PANFROST_PARAM_*, in that order)                  */

bool
redwood_mali_param(uint32_t param, uint64_t *value)
{
	switch (param) {
	case 0: *value = hw.gpu_id >> 16; break;                        /* GPU_PROD_ID */
	case 1: *value = hw.gpu_id & 0xffff; break;                     /* GPU_REVISION */
	case 2: *value = hw.shader_lo | ((uint64_t)hw.shader_hi << 32); break;
	case 3: *value = hw.tiler; break;
	case 4: *value = hw.l2; break;
	case 5: *value = hw.stack; break;
	case 6: *value = hw.as_present; break;
	case 7: *value = hw.js_present; break;
	case 8: *value = hw.l2_features; break;
	case 9: *value = hw.core_features; break;
	case 10: *value = hw.tiler_features; break;
	case 11: *value = hw.mem_features; break;
	case 12: *value = hw.mmu_features; break;
	case 13: *value = hw.thread_features; break;
	case 14: *value = hw.thread_max; break;
	case 15: *value = hw.thread_wg; break;
	case 16: *value = hw.thread_barrier; break;
	case 17: *value = hw.coherency; break;
	case 18: case 19: case 20: case 21: *value = hw.texture[param - 18]; break;
	case 22 ... 37: *value = hw.js[param - 22]; break;
	case 38: *value = 1; break;                                     /* NR_CORE_GROUPS */
	case 39: *value = hw.tls_alloc; break;
	case 40: *value = hw.afbc; break;
	case 41:                                                        /* SYSTEM_TIMESTAMP */
		if (!mali_up) {
			return false;
		}
		*value = mr(GPU_TIMESTAMP_LO) | ((uint64_t)mr(GPU_TIMESTAMP_HI) << 32);
		break;
	case 42: *value = 26000000; break;                              /* the SoC's system counter */
	case 43: *value = 1U << 1; break;                               /* ALLOWED_JM_CTX_PRIORITIES: medium */
	case 44: *value = 31; break;                                    /* SELECTED_COHERENCY: none */
	default:
		return false;
	}
	return true;
}

bool
redwood_mali_ready(void)
{
	return mali_up;
}

/* ------------------------------------------------------------------------ */

/* mali_selftest=1: a WRITE_VALUE job (Mesa genxml v9.xml) through the whole of the above. */
static void
selftest(void)
{
	vm_offset_t page = 0;
	uint64_t pa, va, seqno;
	volatile uint64_t *result;
	uint32_t *job;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return;
	}
	pa = kvtophys(page);
	va = redwood_mali_va_alloc(4096, false);
	if (va == 0 || !redwood_mali_map(va, pa, REDWOOD_MALI_MAP_NOEXEC)) {
		printf("mali: self test: no GPU address\n");
		return;
	}
	redwood_mali_mapped(va, 4096);
	job = (uint32_t *)page;
	job[4] = (2U << 1) | (1U << 16);                /* WRITE_VALUE, index 1 */
	*(uint64_t *)(page + 32) = va + 0x800;
	job[10] = 7;                                    /* immediate, 64 bits */
	*(uint64_t *)(page + 48) = 0xfeedfacecafebeefULL;
	result = (volatile uint64_t *)(page + 0x800);
	clean_lines((void *)page, 4096, true);
	seqno = redwood_mali_submit(va, 0);
	bool done = redwood_mali_wait(seqno, 1000);
	clean_lines((void *)page, 4096, true);
	printf("mali: self test: job %llu %s, wrote %#llx (%s), exception status %#x\n", seqno, done ? "done" : "NOT DONE", *result,
	    *result == 0xfeedfacecafebeefULL ? "right" : "WRONG", job[0]);
	redwood_mali_unmap(va, 4096);
	redwood_mali_va_free(va, 4096);
}

static void
bringup(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t test = 0;
	thread_t thread;

	PE_parse_boot_argn("mali_selftest", &test, sizeof(test));
	PE_parse_boot_argn("mali_trace", &trace, sizeof(trace));
	pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x10000);         /* (the atomic set alias: offset | 0xc000) */
	cmu_top = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x1000);
	cmu_g3d = (volatile uint8_t *)ml_io_map(CMU_G3D_PHYS, 0x1000);
	if (!power_cycle()) {
		return;
	}
	mali = (volatile uint8_t *)ml_io_map(MALI_PHYS, MALI_SIZE);
	read_features();
	PE_parse_boot_argn("mali_cached", &mali_cached, sizeof(mali_cached));
	mali_log("mali: Mali-G77 GPU_ID %#x, shader cores %#x (%u), L2 %#x, tiler %#x, AS %#x, JS %#x, MMU %#x; memory %s\n", hw.gpu_id,
	    hw.shader_lo, __builtin_popcount(hw.shader_lo) + __builtin_popcount(hw.shader_hi), hw.l2, hw.tiler, hw.as_present,
	    hw.js_present, hw.mmu_features, mali_cached ? "write-back in the GPU's L2 (kbase's 0x8d; mali_cached=0: uncached)" :
	    "uncached (mali_cached=0)");

	va_free_list[0].start = VA_START;
	va_free_list[0].size = VA_END - VA_START;
	va_free_count = 1;
	lck_mtx_lock(&mali_lock);
	pt_root = node_new(false);
	mali_up = pt_root != NULL && hw_init("bring-up");
	lck_mtx_unlock(&mali_lock);
	if (!mali_up) {
		mali_log("mali: bring-up failed\n");
		return;
	}
	{
		uint32_t irq = 0;

		PE_parse_boot_argn("mali_irq", &irq, sizeof(irq));
		if (irq != 0) {
			kern_return_t kr = gic_v3_spi_attach(MALI_JOB_INTID, false, job_irq, NULL);

			use_irq = kr == KERN_SUCCESS;
			mali_log("mali: job interrupt (GIC %u): %s\n", MALI_JOB_INTID,
			    use_irq ? "attached, the scheduler sleeps until it" : "NOT attached, polling");
		}
	}
	if (kernel_thread_start(scheduler, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	printf("mali: up: address space 0 at %#llx, GPU addresses %#llx..%#llx\n", kvtophys((vm_offset_t)pt_root->table), VA_START,
	    VA_END);
	if (test) {
		selftest();
	}
	RedwoodMaliStart();
}

/*
 * On the way back to the loader (osfmk/arm64/loader_return.c, the other processors stopped): the GPU's
 * power domain off, its PLL and clock mux off, as power_cycle() does before it powers it up -- an idle G3D
 * at 702 MHz kept the phone warm in the loader's menu, and its page tables are in memory the next kernel
 * reuses. No locks, no printing: the next bring-up powers it on again either way.
 */
void redwood_mali_quiesce(void);

void
redwood_mali_quiesce(void)
{
	uint64_t t;

	if (pmu == NULL || cmu_g3d == NULL || cmu_top == NULL) {
		return;
	}
	mali_up = false;
	if (g3d_on()) {
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~1U);
		for (t = mach_absolute_time(); g3d_on() && mach_absolute_time() - t < 24000000ULL;) {   /* (~1 s at 24 MHz) */
			__builtin_arm_yield();
		}
	}
	if (!g3d_on()) {
		wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) & ~(1U << 4));
		wr(cmu_top, PLL_CON0_PLL_G3D, rd(cmu_top, PLL_CON0_PLL_G3D) & ~(1U << 4));
	}
}

void
redwood_mali_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mali", &mode, sizeof(mode)) || mode == 0) {
		return;
	}
	if (kernel_thread_start(bringup, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
