/*
 * redwood_exynos_acpm.c: the clocks of the Exynos 990's processors, GPU and memory, set through
 * Samsung's ACPM (the power-management core that owns the PLLs and the regulators), and the
 * temperatures it reads -- PLANS/01-performance.md P5, PLANS/04-gpu.md G8.
 *
 * Until this, every clock was what S-Boot left: the A55s at ~1.05 GHz, the A76s at ~1.07 GHz, the
 * Mongoose M5s at 266 MHz (so slow they are left out: cpumask=0x3f), the GPU at its boot clock.
 * No register of a PLL is touched here; ACPM does that, voltage first, on request.
 *
 * The protocol is Samsung's (android_kernel_samsung_exynos990: drivers/soc/samsung/acpm/acpm_ipc.c,
 * drivers/soc/samsung/cal-if/acpm_dvfs.c, drivers/thermal/samsung/exynos_acpm_tmu.c), and every
 * number in this file comes from the phone's own device tree (research/dts/dtb.img.0.dts):
 *   acpm_ipc   mailbox 0x15900000, SRAM 0x02039000 (0x38000), initdata-base 0x7000
 *   acpm_dvfs  channel 5: { domain, kHz, FREQ_REQ 0 | FREQ_GET 1, 0 }
 *   acpm_tmu   channel 9: READ_TEMP 0x02 for zone BIG 0, MID 1, LITTLE 2, G3D 3
 *   domains    MIF 0, INT 1, CL0 2 (cpus 0-3, A55), CL1 3 (4-5, A76), CL2 4 (6-7, M5), DISP 6, G3D 10
 * A channel's queues live in SRAM: the framework at initdata-base names an array of channel
 * descriptors; the AP writes a command into the descriptor's rx queue, bumps its front, rings
 * INTGR0 (bit 16 + id), and the answer appears in the tx queue with the same sequence number.
 *
 * Boot args:
 *   dvfs=1       read: every domain's clock and the temperatures, then every 10 s
 *   dvfs=2       ... and set the clocks below (kHz; 0 leaves a domain as it is):
 *                dvfs_cl0 (A55, default 1742000), dvfs_cl1 (A76, 2210000), dvfs_cl2 (M5, 2002000),
 *                dvfs_g3d (GPU, 702000), dvfs_mif (memory, 0)
 *   dvfs_wait=S  seconds after boot before anything is asked (default 10)
 *   dvfs_hot=C   the first step of each domain's throttling, in degrees of its own zone (default:
 *                Samsung's: A55 86 / 96 by the LITTLE zone, A76 86 / 96 by MID, GPU 88 / 100 by G3D,
 *                exynos9830.dts thermal-zones); a step is let go 6 degrees below where it was taken.
 *                Step 1 is the "mid" clock, step 2 the safe one. Only the domain that is hot slows.
 *   fb_hot=C     the panel held under 70 % of its brightness from C degrees in the hottest zone (default
 *                90; 0: never), under 50 % from C + 6; the 50 % let go below C + 2, the 70 % below C - 4.
 *                The panel's power is heat in the same body as the SoC (a phone's thermal policy does the
 *                same; Samsung's is in user space, SSRM, so these marks are this driver's own).
 * Every line goes to the loader's log with a `dvfs:` prefix.
 */

#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <sys/redwood_m2m.h>
#include <mach/processor_info.h>
#include <mach/machine.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern unsigned int real_ncpus;
extern processor_t processor_array[];
extern kern_return_t processor_info(processor_t processor, processor_flavor_t flavor, host_t *host,
    processor_info_t info, mach_msg_type_number_t *count);
extern uint32_t processor_avail_count;

#define ACPM_INTR_PHYS          0x15900000UL
#define ACPM_SRAM_PHYS          0x02039000UL
#define ACPM_SRAM_SIZE          0x38000
#define ACPM_INITDATA           0x7000

#define INTGR0                  0x08            /* AP -> APM: bit 16 + channel id */
#define INTCR1                  0x20            /* APM -> AP: clear */
#define INTMR1                  0x24            /*            mask (1 = no interrupt: we poll) */
#define INTSR1                  0x28            /*            status */

#define CH_DVFS                 5
#define CH_TMU                  9

#define FREQ_REQ                0
#define FREQ_GET                1
#define TMU_IPC_INIT            0x01
#define TMU_IPC_READ_TEMP       0x02

#define TYPE_BUFFER             2               /* framework.h: one slot, no ring (TYPE_QUEUE 1) */

#define CMU_TOP_PHYS            0x1a330000UL
#define PLL_CON3_PLL_G3D        0x10c           /* ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0; 26 MHz in */

#define SEQ_SHIFT               16
#define SEQ_MASK                (0x3fu << SEQ_SHIFT)

enum { D_MIF = 0, D_INT = 1, D_CL0 = 2, D_CL1 = 3, D_CL2 = 4, D_DISP = 6, D_G3D = 10 };

static const struct {
	unsigned domain;
	const char *name;
	const char *arg;
	uint32_t target;        /* kHz, dvfs=2's default */
	uint32_t safe;          /* kHz, when too hot */
	uint32_t mid;           /* kHz, the first throttling step */
	int zone;               /* the zone that throttles it (zones[]), -1: none */
	int step1, step2;       /* degrees */
} domains[] = {
	{ D_CL0, "cl0 (A55)", "dvfs_cl0", 1742000, 1053000, 1352000, 2, 86, 96 },
	{ D_CL1, "cl1 (A76)", "dvfs_cl1", 2210000, 1066000, 1612000, 1, 86, 96 },
	{ D_CL2, "cl2 (M5)",  "dvfs_cl2", 0,       1196000, 1794000, 0, 86, 96 },  /* only with dvfs_cl2= and every core up */
	{ D_G3D, "g3d",       "dvfs_g3d", 702000,  377000,  572000,  3, 88, 100 },
	{ D_MIF, "mif",       "dvfs_mif", 0,       0,       0,       -1, 0, 0 },
	{ D_INT, "int",       NULL,       0,       0,       0,       -1, 0, 0 },
	{ D_DISP, "disp",     NULL,       0,       0,       0,       -1, 0, 0 },
};
#define NDOMAINS        (sizeof(domains) / sizeof(domains[0]))

static const char *const zones[] = { "big", "mid", "little", "g3d" };

/* one channel, as acpm_ipc.c's channel_init() reads it out of SRAM */
struct channel {
	uint32_t id, polling, elem, len, type;
	volatile uint8_t *tx_front, *tx_rear, *tx_base;        /* AP -> APM (the descriptor's "rx") */
	volatile uint8_t *rx_front, *rx_rear, *rx_base;        /* APM -> AP (the descriptor's "tx") */
	uint32_t seq;
};

static volatile uint8_t *intr, *sram, *cmu_top;
static struct channel ch_dvfs, ch_tmu;
static uint32_t targets[NDOMAINS];

static void dlog(const char *format, ...) __printflike(1, 2);

static void
dlog(const char *format, ...)
{
	char line[320];
	va_list list;
	size_t len;

	strlcpy(line, "dvfs: ", sizeof(line));
	va_start(list, format);
	vsnprintf(line + 6, sizeof(line) - 7, format, list);
	va_end(list);
	/* into mali2metal's channel as well, but not each clock request (the governor's, several a second) */
	if (strncmp(line + 6, "domain ", 7) != 0) {
		m2m_klog('I', "PERF", "%s", line);
	}
	len = strlen(line);
	line[len] = '\n';
	line[len + 1] = '\0';
	loader_log_line(line);
}

static inline uint32_t
rd(volatile uint8_t *at)
{
	return *(volatile uint32_t *)at;
}

static inline void
wr(volatile uint8_t *at, uint32_t value)
{
	*(volatile uint32_t *)at = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The descriptor of the channel whose id is `want` (acpm_ipc_request_channel looks channels up by
 * id, not by place). struct ipc_channel { id, field, owner, type, channel_info ch, ap_poll } with
 * channel_info { rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size,
 * tx_rear, tx_front, tx_base, q_len, q_elem_size, credit, mbox_addr }: 18 words.
 */
static int
find_channel(uint32_t want, struct channel *c)
{
	uint32_t channels = rd(sram + ACPM_INITDATA + 8);
	uint32_t count = rd(sram + ACPM_INITDATA + 24);         /* ipc_ap_max */

	if (channels == 0 || channels >= ACPM_SRAM_SIZE || count == 0 || count > 32) {
		dlog("the framework at SRAM+%#x names %u channels at %#x: not what was expected",
		    ACPM_INITDATA, count, channels);
		return -1;
	}
	/*
	 * Linux finds the channel whose id is `want` and then sends through acpm_ipc->channel[id], the
	 * descriptor at POSITION `want` (acpm_ipc_request_channel returns the id; __acpm_ipc_send_data
	 * indexes the array with it). Looking the descriptor up by its id instead found channel 5 with a
	 * queue of one entry, which no ring can use (25 October). So: the descriptor at that position.
	 */
	for (uint32_t i = 0; i < count; i++) {
		volatile uint8_t *d = sram + channels + i * 72;

		if (want == CH_DVFS) {
			dlog("descriptor %u: id %u field %u owner %d type %u, queue %u x %u, polled %u, to ACPM %#x/%#x/%#x, "
			    "from ACPM %#x/%#x/%#x", i, rd(d), rd(d + 4), (int)rd(d + 8), rd(d + 12), rd(d + 52), rd(d + 56),
			    rd(d + 68), rd(d + 16), rd(d + 20), rd(d + 24), rd(d + 40), rd(d + 44), rd(d + 48));
		}
	}
	for (uint32_t i = want; i < count; i = count) {
		volatile uint8_t *d = sram + channels + i * 72;
		uint32_t rx_rear = rd(d + 16), rx_front = rd(d + 20), rx_base = rd(d + 24);
		uint32_t tx_rear = rd(d + 40), tx_front = rd(d + 44), tx_base = rd(d + 48);

		c->id = rd(d);
		c->len = rd(d + 52);
		c->elem = rd(d + 56);
		c->polling = rd(d + 68);
		c->type = rd(d + 12);
		if (c->len == 0 || c->len > 64 || c->elem < 16 || c->elem > 64 ||
		    rx_base >= ACPM_SRAM_SIZE || tx_base >= ACPM_SRAM_SIZE ||
		    rx_front >= ACPM_SRAM_SIZE || tx_front >= ACPM_SRAM_SIZE) {
			dlog("channel %u: queue of %u x %u bytes, offsets rx %#x/%#x/%#x tx %#x/%#x/%#x: "
			    "out of range", want, c->len, c->elem, rx_rear, rx_front, rx_base, tx_rear,
			    tx_front, tx_base);
			return -1;
		}
		c->tx_rear = sram + rx_rear;
		c->tx_front = sram + rx_front;
		c->tx_base = sram + rx_base;
		c->rx_rear = sram + tx_rear;
		c->rx_front = sram + tx_front;
		c->rx_base = sram + tx_base;
		c->seq = 0;
		dlog("channel %u (descriptor %u, id %u): %s, %u x %u bytes, %s; to ACPM front %u rear %u, from ACPM front %u rear %u",
		    want, i, c->id, c->type == TYPE_BUFFER ? "buffer" : "queue", c->len, c->elem,
		    c->polling ? "polled" : "interrupt-driven", rd(c->tx_front),
		    rd(c->tx_rear), rd(c->rx_front), rd(c->rx_rear));
		return 0;
	}
	dlog("no descriptor %u among the %u the framework names", want, count);
	return -1;
}

/* One command and its answer (acpm_ipc.c __acpm_ipc_send_data + check_response). 0, or -1. */
static int
ipc(struct channel *c, uint32_t cmd[4])
{
	uint32_t front = rd(c->tx_front), next = front + 1 == c->len ? 0 : front + 1;
	uint32_t seq, i;
	uint64_t deadline;

	/*
	 * A buffer channel (DVFS, 5) has one slot and no ring: front and rear stay 0, so the ring's
	 * "full" test is always true for it. acpm_ipc.c applies that test anyway, but its waits time
	 * out on our side of the firmware's start-up. Linux does not wait for an answer on it either
	 * (not polled: the answer is an interrupt and a callback): the command is written to slot 0,
	 * the doorbell rung, and the answer taken if one comes in 20 ms (1: sent, no answer).
	 */
	if (c->type == TYPE_BUFFER) {
		front = 0;
		next = 0;
	} else if (next == rd(c->tx_rear)) {
		dlog("channel %u: the queue to ACPM is full (front %u rear %u)", c->id, front, rd(c->tx_rear));
		return -1;
	}
	seq = c->seq = c->seq >= 63 ? 1 : c->seq + 1;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | (seq << SEQ_SHIFT);
	for (i = 0; i < c->elem / 4; i++) {
		wr(c->tx_base + c->elem * front + i * 4, i < 4 ? cmd[i] : 0);
	}
	wr(c->tx_front, next);
	wr(intr + INTGR0, (1u << c->id) << 16);

	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(intr + INTSR1) & (1u << c->id)) && c->type == TYPE_BUFFER) {
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = rd(c->rx_base + w * 4);
			}
			wr(intr + INTCR1, 1u << c->id);
			return 0;
		}
		if (rd(intr + INTSR1) & (1u << c->id)) {
			uint32_t rear = rd(c->rx_rear), last = rd(c->rx_front);

			for (i = rear; i != last; i = i + 1 == c->len ? 0 : i + 1) {
				volatile uint8_t *e = c->rx_base + c->elem * i;

				if (((rd(e) & SEQ_MASK) >> SEQ_SHIFT) != seq) {
					continue;
				}
				for (uint32_t w = 0; w < 4; w++) {
					cmd[w] = rd(e + w * 4);
				}
				if (i != rear) {        /* keep the queue dense: the rear's entry takes this place */
					for (uint32_t w = 0; w < c->elem / 4; w++) {
						wr(e + w * 4, rd(c->rx_base + c->elem * rear + w * 4));
					}
				}
				rear = rear + 1 == c->len ? 0 : rear + 1;
				wr(c->rx_rear, rear);
				if (rear == rd(c->rx_front)) {
					wr(intr + INTCR1, 1u << c->id);
				}
				return 0;
			}
		}
		delay(10);
	}
	if (c->type == TYPE_BUFFER) {
		return 1;
	}
	dlog("channel %u: no answer to %#x %#x %#x %#x in 20 ms (status %#x, from ACPM front %u rear %u)",
	    c->id, cmd[0], cmd[1], cmd[2], cmd[3], rd(intr + INTSR1), rd(c->rx_front), rd(c->rx_rear));
	return -1;
}

static int
set_rate(unsigned domain, uint32_t khz)
{
	uint32_t cmd[4] = { domain, khz, FREQ_REQ, 0 };
	int ret = ipc(&ch_dvfs, cmd);

	dlog("domain %u to %u kHz: %s (%#x %#x %#x %#x)", domain, khz, ret == 0 ? "answered" : ret == 1 ? "sent, no answer" :
	    "NOT sent", cmd[0], cmd[1], cmd[2], cmd[3]);
	return ret < 0 ? -1 : 0;
}

/* what PLL_G3D really runs at: the proof that a request was taken */
static uint32_t
g3d_pll_mhz(void)
{
	uint32_t con3 = cmu_top ? rd(cmu_top + PLL_CON3_PLL_G3D) : 0, m = (con3 >> 16) & 0x3ff, p = (con3 >> 8) & 0x3f;

	return p ? 26 * m / (p << (con3 & 7)) : 0;
}

/* degrees C, or -1 (exynos_acpm_tmu.c: data[1] = type | tzid << 16, answer's temp in byte 3) */
static int
read_temp(unsigned zone)
{
	uint32_t cmd[4] = { 0, TMU_IPC_READ_TEMP | (zone << 16), 0, 0 };

	if (ipc(&ch_tmu, cmd) != 0 || (int8_t)(cmd[1] >> 8) != 0) {
		return -1;
	}
	return (int)((cmd[1] >> 24) & 0xff);
}

static void
report(const char *why)
{
	char line[300];
	size_t at = 0;

	/* (ACPM does not answer FREQ_GET as acpm_dvfs.c asks it: every domain read 0, run 20) */
	at += snprintf(line + at, sizeof(line) - at, "PLL_G3D %u MHz", g3d_pll_mhz());
	if (ch_tmu.len != 0) {
		at += snprintf(line + at, sizeof(line) - at, "; temperatures");
		for (unsigned z = 0; z < sizeof(zones) / sizeof(zones[0]) && at < sizeof(line) - 16; z++) {
			at += snprintf(line + at, sizeof(line) - at, " %s %d", zones[z], read_temp(z));
		}
	}
	dlog("%s: %s", why, line);
}

static void
apply(int hot, bool skip_g3d)
{
	for (unsigned i = 0; i < NDOMAINS; i++) {
		uint32_t khz = hot ? domains[i].safe : targets[i];

		/* (cluster 2 is asked on its own, last: acpm_thread) */
		if (khz == 0 || (skip_g3d && domains[i].domain == D_G3D) || domains[i].domain == D_CL2) {
			continue;
		}
		if (set_rate(domains[i].domain, khz) != 0) {
			dlog("%s: %u kHz was not taken", domains[i].name, khz);
		}
		if (domains[i].domain == D_G3D && cmu_top != NULL) {
			delay(2000);
			dlog("g3d asked for %u MHz: PLL_G3D now %u MHz", khz / 1000, g3d_pll_mhz());
		}
	}
}

static int
hottest(void)
{
	int worst = -1;

	for (unsigned z = 0; ch_tmu.len != 0 && z < sizeof(zones) / sizeof(zones[0]); z++) {
		int t = read_temp(z);

		worst = t > worst ? t : worst;
	}
	return worst;
}

/*
 * dvfs_gov=1 (default with dvfs=2): the CPU clusters' clocks follow their load, as a phone's do. Pinned at
 * their tops they ran 66-75 C with the GPU idle and hit 105 C (the M5s) at boot. Every 200 ms: each
 * cluster's busiest core; over 70 % busy the top rung at once (a touch must not wait for the clock),
 * otherwise the rung that would make it ~60 % busy, coming down one rung at a time after 0.6 s. The
 * thermal steps cap it.
 *
 * The GPU (dvfs_gov_gpu=1, the default) the same way from its own busy time (redwood_mali_busy_pct), over
 * Samsung's table (dtb gpu_dvfs_table: 156 to 702 MHz here; 800 and 832 are not used), but aiming at ~40 %
 * busy and jumping to the top at 60 %: the window server waits for the GPU in every swap, so a GPU kept
 * busier is a longer frame. It ran at 702 MHz 90-96 % idle (runs 1-31), the SoC at 81 C idle.
 *
 * A touch: Samsung's input booster for this phone (dts input_booster, booster_key@3 "touch": head_time
 * 130 ms, tail_time 500 ms) holds each cluster at least at its head clock for 130 ms after a finger comes
 * down and at its tail clock until 500 ms after the last touch event; here each as the lowest rung at or
 * above it, applied the moment the finger lands (the governor is woken) rather than at the next tick.
 * The booster's MIF (memory, 1014 MHz) floor is not applied: the memory's clock is not driven here.
 * The booster sets no GPU floor; this one holds the GPU at 572 MHz for a touch: measured cold (27 October)
 * a heavy frame's fragment jobs ran 17-23 ms at 156-260 MHz, and 572 is the lowest of Samsung's rungs that
 * would bring them near 8 ms by the clock alone (an estimate: the memory's speed may cap it).
 *
 * A GPU job longer than half a refresh (redwood_mali.c: redwood_mali_slow_jobs, which wakes this thread)
 * takes the GPU to its top rung at once; it comes down as from any other jump.
 */
int redwood_gov_wake;                   /* the event a touch or a slow GPU job wakes the governor with */
#define GOV_TICK_MS     200
#define GOV_RUNGS       6
static const struct {
	unsigned domain;
	unsigned first_cpu, ncpus;      /* ncpus 0: the GPU */
	uint32_t rung[GOV_RUNGS];       /* kHz, low to high; 0: unused */
	unsigned aim, jump;             /* the busy % a rung is chosen for, and the % that takes the top at once */
	uint32_t touch_head, touch_tail;        /* kHz the input booster holds at least (0: none) */
} gov[] = {
	{ D_CL0, 0, 4, { 806000, 1053000, 1352000, 1742000, 0, 0 }, 60, 70, 806000, 806000 },
	{ D_CL1, 4, 2, { 1066000, 1612000, 2210000, 0, 0, 0 }, 60, 70, 1404000, 1222000 },
	{ D_CL2, 6, 2, { 1196000, 1794000, 2304000, 0, 0, 0 }, 60, 70, 1066000, 858000 },
	{ D_G3D, 0, 0, { 156000, 260000, 377000, 455000, 572000, 702000 }, 40, 60, 572000, 572000 },
};
#define TOUCH_HEAD_MS   130
#define TOUCH_TAIL_MS   500
extern uint64_t redwood_touch_down_at, redwood_touch_last_at;
extern uint32_t redwood_mali_slow_jobs;

/* the lowest rung the touch booster holds cluster `g` at now (0: no boost) */
static unsigned
gov_touch_floor(unsigned g)
{
	uint64_t now = mach_absolute_time(), head, tail;
	uint32_t khz;
	unsigned r = 0;

	nanoseconds_to_absolutetime(TOUCH_HEAD_MS * NSEC_PER_MSEC, &head);
	nanoseconds_to_absolutetime(TOUCH_TAIL_MS * NSEC_PER_MSEC, &tail);
	khz = redwood_touch_down_at != 0 && now - redwood_touch_down_at < head ? gov[g].touch_head :
	    redwood_touch_last_at != 0 && now - redwood_touch_last_at < tail ? gov[g].touch_tail : 0;
	while (khz != 0 && r + 1 < GOV_RUNGS && gov[g].rung[r + 1] != 0 && gov[g].rung[r] < khz) {
		r++;
	}
	return r;
}
extern int redwood_mali_busy_pct(void);
#define NGOV            (sizeof(gov) / sizeof(gov[0]))

static uint32_t gov_ticks[8][2];        /* busy, all: the last reading of each core */

/* the busiest core of the cluster since the last call (or the GPU's load), in percent; -1: no reading */
static int
gov_busy(unsigned g)
{
	int worst = -1;

	if (gov[g].ncpus == 0) {
		return redwood_mali_busy_pct();
	}

	for (unsigned c = gov[g].first_cpu; c < gov[g].first_cpu + gov[g].ncpus && c < 8 && c < real_ncpus; c++) {
		processor_cpu_load_info_data_t load;
		mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
		host_t host;
		uint32_t busy, all, db, da;

		if (processor_array[c] == PROCESSOR_NULL ||
		    processor_info(processor_array[c], PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&load, &count) != KERN_SUCCESS) {
			continue;
		}
		busy = load.cpu_ticks[CPU_STATE_USER] + load.cpu_ticks[CPU_STATE_SYSTEM] + load.cpu_ticks[CPU_STATE_NICE];
		all = busy + load.cpu_ticks[CPU_STATE_IDLE];
		db = busy - gov_ticks[c][0];
		da = all - gov_ticks[c][1];
		gov_ticks[c][0] = busy;
		gov_ticks[c][1] = all;
		if (da != 0) {
			int pct = (int)(db * 100 / da);

			worst = pct > worst ? pct : worst;
		}
	}
	return worst;
}

static void
acpm_thread(void *arg, wait_result_t wr_unused)
{
	uint32_t mode = 0, wait = 10, hot_at = 0, fb_hot = 90;
	int bl_step = 0;
	int hot = 0, level[NDOMAINS] = { 0 }, temp[4];
	uint32_t govern = 1, govern_gpu = 1, gov_rung[NGOV], gov_low[NGOV], gov_sent[NGOV], gov_changes = 0, tick = 0;
	uint32_t gov_boosts = 0, gov_touches = 0, gov_slow_seen = 0, gov_slow_raises = 0, gov_slow_last = 0;
	uint64_t gov_khz_sum[NGOV];

	(void)arg;
	(void)wr_unused;
	PE_parse_boot_argn("dvfs", &mode, sizeof(mode));
	PE_parse_boot_argn("dvfs_wait", &wait, sizeof(wait));
	PE_parse_boot_argn("dvfs_hot", &hot_at, sizeof(hot_at));
	PE_parse_boot_argn("dvfs_gov", &govern, sizeof(govern));
	PE_parse_boot_argn("fb_hot", &fb_hot, sizeof(fb_hot));
	PE_parse_boot_argn("dvfs_gov_gpu", &govern_gpu, sizeof(govern_gpu));
	for (unsigned g = 0; g < NGOV; g++) {
		gov_rung[g] = GOV_RUNGS - 1;
		while (gov_rung[g] > 0 && gov[g].rung[gov_rung[g]] == 0) {
			gov_rung[g]--;
		}
		gov_low[g] = 0;
		gov_sent[g] = 0;
		gov_khz_sum[g] = 0;
	}
	for (unsigned i = 0; i < NDOMAINS; i++) {
		targets[i] = domains[i].target;
		if (domains[i].arg != NULL) {
			PE_parse_boot_argn(domains[i].arg, &targets[i], sizeof(targets[i]));
		}
	}
	delay_for_interval(wait, NSEC_PER_SEC);
	/*
	 * Every core up first: a core is powered on through the firmware, and a clock change in
	 * flight there held cpu1-3 off for good once (25 October, with cluster 2's request hung).
	 */
	for (unsigned waited = 0; processor_avail_count < real_ncpus && waited < 120; waited++) {
		delay_for_interval(1, NSEC_PER_SEC);
	}
	if (processor_avail_count < real_ncpus) {
		dlog("only %u of %u cores up after two minutes: clocks are left alone", processor_avail_count, real_ncpus);
		mode = mode > 1 ? 1 : mode;
	}

	intr = (volatile uint8_t *)ml_io_map(ACPM_INTR_PHYS, 0x1000);
	sram = (volatile uint8_t *)ml_io_map(ACPM_SRAM_PHYS, ACPM_SRAM_SIZE);
	if (intr == NULL || sram == NULL) {
		dlog("the ACPM mailbox or its SRAM could not be mapped");
		return;
	}
	dlog("dvfs=%u: ACPM framework at SRAM+%#x: %u plugins, %u channels (%u for the AP)", mode,
	    ACPM_INITDATA, rd(sram + ACPM_INITDATA + 4), rd(sram + ACPM_INITDATA + 12),
	    rd(sram + ACPM_INITDATA + 24));
	if (find_channel(CH_DVFS, &ch_dvfs) != 0) {
		return;
	}
	if (find_channel(CH_TMU, &ch_tmu) != 0) {
		ch_tmu.len = 0;
	}
	/* we poll: no interrupt to the AP for these two (INTMR1 bit set = masked) */
	wr(intr + INTMR1, rd(intr + INTMR1) | (1u << CH_DVFS) | (ch_tmu.len ? 1u << CH_TMU : 0));
	if (ch_tmu.len != 0) {
		uint32_t cmd[4] = { 0, TMU_IPC_INIT, 0, 0 };

		if (ipc(&ch_tmu, cmd) != 0) {
			ch_tmu.len = 0;
		}
	}
	cmu_top = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x1000);
	report("as S-Boot left them");
	if (mode >= 2) {
		/* the GPU first, on its own: the CPUs only if PLL_G3D shows ACPM took it */
		uint32_t before = g3d_pll_mhz(), g3d = 0;

		for (unsigned i = 0; i < NDOMAINS; i++) {
			if (domains[i].domain == D_G3D) {
				g3d = targets[i];
			}
		}
		set_rate(D_G3D, g3d);
		delay(5000);
		if (g3d == 0 || g3d_pll_mhz() == before) {
			dlog("PLL_G3D still %u MHz after asking for %u: ACPM did not take it, nothing else is set", g3d_pll_mhz(),
			    g3d / 1000);
			mode = 1;
		} else {
			dlog("PLL_G3D %u -> %u MHz: ACPM DVFS works", before, g3d_pll_mhz());
		}
	}
	if (mode >= 2) {
		/*
		 * Cluster 2 (cores 6-7, M5) only when dvfs_cl2= says so: the cores are kept off
		 * (cpumask), and a request for a powered-down cluster was never answered and left
		 * ACPM deaf to everything after it (temperatures included) -- 25 October.
		 */
		apply(0, true);
		report("after the request");
		if (ch_tmu.len != 0 && hottest() < 0) {
			dlog("ACPM stopped answering after the clock requests: nothing more is asked of it");
			mode = 1;
		}
	}
	/*
	 * Cluster 2 (the two Mongoose M5, S-Boot leaves them at 266 MHz) last and alone, and only with every
	 * core up -- the one request ever left unanswered was for this cluster while it was powered down.
	 */
	for (unsigned i = 0; mode >= 2 && i < NDOMAINS; i++) {
		if (domains[i].domain != D_CL2 || targets[i] == 0) {
			continue;
		}
		if (processor_avail_count < real_ncpus) {
			dlog("cl2 (M5): not every core is up (%u of %u): its clock is left as S-Boot set it", processor_avail_count,
			    real_ncpus);
			break;
		}
		if (set_rate(D_CL2, targets[i]) != 0) {
			dlog("cl2 (M5): %u kHz was not taken", targets[i]);
		} else {
			dlog("cl2 (M5): asked for %u MHz", targets[i] / 1000);
		}
		delay(5000);
		if (ch_tmu.len != 0 && hottest() < 0) {
			dlog("ACPM STOPPED ANSWERING AFTER THE CLUSTER 2 REQUEST: no temperatures, no throttling from here on");
			mode = 1;
		}
		report("after cluster 2");
	}
	for (;; tick++) {
		uint64_t tick_end;

		/* a tick, or sooner a finger coming down or a slow GPU job: its boost at once */
		clock_interval_to_deadline(GOV_TICK_MS, NSEC_PER_MSEC, &tick_end);
		for (;;) {
			bool slow;

			assert_wait_deadline((event_t)&redwood_gov_wake, THREAD_UNINT, tick_end);
			if (thread_block(THREAD_CONTINUE_NULL) != THREAD_AWAKENED) {
				break;
			}
			slow = redwood_mali_slow_jobs != gov_slow_seen;
			gov_slow_seen = redwood_mali_slow_jobs;
			gov_touches += !slow;
			for (unsigned g = 0; govern && mode >= 2 && g < NGOV; g++) {
				unsigned d, floor = gov_touch_floor(g);
				uint32_t cap, khz;

				if (slow && gov[g].ncpus == 0) {
					for (floor = GOV_RUNGS - 1; floor > 0 && gov[g].rung[floor] == 0; floor--) {
					}
					gov_slow_raises += floor > gov_rung[g];
				}

				for (d = 0; d < NDOMAINS && domains[d].domain != gov[g].domain; d++) {
				}
				if (d == NDOMAINS || targets[d] == 0 || floor <= gov_rung[g] || (gov[g].ncpus == 0 && !govern_gpu)) {
					continue;
				}
				gov_rung[g] = floor;
				gov_low[g] = 0;
				cap = level[d] == 2 ? domains[d].safe : level[d] == 1 ? domains[d].mid : targets[d];
				khz = gov[g].rung[floor];
				khz = khz > targets[d] ? targets[d] : khz;
				khz = cap != 0 && khz > cap ? cap : khz;
				if (khz != gov_sent[g] && set_rate(domains[d].domain, khz) == 0) {
					gov_sent[g] = khz;
					gov_changes++;
					gov_boosts++;
				}
			}
		}
		/* the governor: every tick */
		for (unsigned g = 0; govern && mode >= 2 && g < NGOV; g++) {
			unsigned d = 0, top = GOV_RUNGS - 1, want;
			int busy;
			uint32_t cap, khz;

			for (d = 0; d < NDOMAINS && domains[d].domain != gov[g].domain; d++) {
			}
			if (d == NDOMAINS || targets[d] == 0) {
				continue;
			}
			while (top > 0 && gov[g].rung[top] == 0) {
				top--;
			}
			if (gov[g].ncpus == 0 && !govern_gpu) {
				continue;
			}
			busy = gov_busy(g);
			if (busy < 0) {
				continue;
			}
			want = gov_rung[g];
			if (busy >= (int)gov[g].jump) {
				want = top;
				gov_low[g] = 0;
			} else {
				/* the lowest rung that would leave it at most `aim` % busy */
				uint64_t need = (uint64_t)gov[g].rung[gov_rung[g]] * (uint64_t)busy / gov[g].aim;
				unsigned fit = 0;

				while (fit < top && gov[g].rung[fit] < need) {
					fit++;
				}
				if (fit > gov_rung[g]) {
					want = fit;
					gov_low[g] = 0;
				} else if (fit < gov_rung[g] && ++gov_low[g] >= 3) {
					want = gov_rung[g] - 1;
					gov_low[g] = 0;
				}
			}
			if (want < gov_touch_floor(g)) {
				want = gov_touch_floor(g);
				gov_low[g] = 0;
			}
			gov_rung[g] = want;
			cap = level[d] == 2 ? domains[d].safe : level[d] == 1 ? domains[d].mid : targets[d];
			khz = gov[g].rung[want];
			khz = khz > targets[d] ? targets[d] : khz;
			khz = cap != 0 && khz > cap ? cap : khz;
			gov_khz_sum[g] += khz;
			if (khz != gov_sent[g]) {
				if (set_rate(domains[d].domain, khz) != 0) {
					dlog("%s: %u kHz was not taken", domains[d].name, khz);
				}
				gov_sent[g] = khz;
				gov_changes++;
			}
		}
		if (tick % (1000 / GOV_TICK_MS) != 0) {
			continue;
		}
		/* the temperatures and the thermal steps: every second */
		hot = 0;
		for (unsigned z = 0; z < 4; z++) {
			temp[z] = ch_tmu.len != 0 ? read_temp(z) : -1;
		}
		if (fb_hot != 0) {
			extern void redwood_fb_thermal_cap(uint32_t nits);
			static const uint32_t cap_nits[3] = { 0, 294, 210 };        /* none, 70 %, 50 % of the panel's 420 */
			int t = -1, want = bl_step;

			for (unsigned z = 0; z < 4; z++) {
				t = temp[z] > t ? temp[z] : t;
			}
			/* as the clusters' steps: taken at the mark, let go 4 degrees below it */
			if (t < 0) {
				want = bl_step;
			} else if (t >= (int)fb_hot + 6) {
				want = 2;
			} else if (t >= (int)fb_hot && want < 1) {
				want = 1;
			} else if (want == 2 && t < (int)fb_hot + 2) {
				want = t >= (int)fb_hot - 4 ? 1 : 0;
			} else if (want == 1 && t < (int)fb_hot - 4) {
				want = 0;
			}
			if (want != bl_step) {
				bl_step = want;
				redwood_fb_thermal_cap(cap_nits[want]);
				dlog("display: hottest zone %d C: brightness %s", t, want == 0 ? "as asked again" : want == 1 ?
				    "held under 70 %" : "held under 50 %");
			}
		}
		for (unsigned i = 0; mode >= 2 && i < NDOMAINS; i++) {
			int z = domains[i].zone, t, s1, s2, want;
			uint32_t khz;

			if (z < 0 || targets[i] == 0 || (t = temp[z]) < 0) {
				continue;
			}
			s1 = hot_at ? (int)hot_at : domains[i].step1;
			s2 = hot_at ? (int)hot_at + 10 : domains[i].step2;
			want = level[i];
			if (t >= s2) {
				want = 2;
			} else if (t >= s1 && want < 1) {
				want = 1;
			} else if (want == 2 && t < s2 - 6) {
				want = t >= s1 ? 1 : 0;
			} else if (want == 1 && t < s1 - 6) {
				want = 0;
			}
			if (want != level[i]) {
				khz = want == 2 ? domains[i].safe : want == 1 ? (domains[i].mid < targets[i] ? domains[i].mid : targets[i]) : targets[i];
				level[i] = want;
				/* (a governed cluster gets its clock from the governor, capped by this step, next tick) */
				bool governed = govern && (domains[i].domain == D_CL0 || domains[i].domain == D_CL1 ||
				    domains[i].domain == D_CL2 || (domains[i].domain == D_G3D && govern_gpu));
				if (khz != 0 && !governed && set_rate(domains[i].domain, khz) != 0) {
					dlog("%s: %u kHz was not taken", domains[i].name, khz);
				}
				dlog("%s zone %s %d C: step %d, %u MHz", domains[i].name, zones[z], t, want, khz / 1000);
			}
			hot |= level[i] != 0;
		}
		if (tick % (10000 / GOV_TICK_MS) != 0) {
			continue;
		}
		if (govern && mode >= 2) {
			unsigned n = 10000 / GOV_TICK_MS;

			dlog("governor: 10 s: %u clock changes (%u raised the moment a finger came down, of %u touches; the GPU "
			    "raised to its top %u times by %u jobs over 8 ms); average cl0 %llu cl1 %llu cl2 %llu g3d %llu MHz",
			    gov_changes, gov_boosts, gov_touches, gov_slow_raises, redwood_mali_slow_jobs - gov_slow_last,
			    gov_khz_sum[0] / n / 1000, gov_khz_sum[1] / n / 1000, gov_khz_sum[2] / n / 1000, gov_khz_sum[3] / n / 1000);
			gov_boosts = gov_touches = gov_slow_raises = 0;
			gov_slow_last = redwood_mali_slow_jobs;
			gov_changes = 0;
			for (unsigned g = 0; g < NGOV; g++) {
				gov_khz_sum[g] = 0;
			}
		}
		{
			extern uint64_t redwood_user_cache_ops, redwood_user_cache_flushes;
			static uint64_t last_ops, last_flushes;
			uint64_t ops = redwood_user_cache_ops, flushes = redwood_user_cache_flushes;

			extern uint64_t redwood_user_cache_small;
			static uint64_t last_small;
			uint64_t small = redwood_user_cache_small;

			dlog("user cache maintenance done for user code (trapped, osfmk/arm64/sleh.c): %llu in 10 s, of them "
			    "%llu whole sys_icache_invalidate ranges (every core's instruction cache; %llu line by line through "
			    "the kernel's alias, the rest IC IALLU)", ops - last_ops, flushes - last_flushes, small - last_small);
			last_small = small;
			last_ops = ops;
			last_flushes = flushes;
		}
		{
			struct cpuidle_count { uint64_t entered, resumed, returned, refused, skipped, slept_abs; };
			extern struct cpuidle_count redwood_cpuidle_counts[8];
			static struct cpuidle_count last[8];
			char line[512];
			size_t at = 0;
			uint64_t any = 0;

			at += snprintf(line + at, sizeof(line) - at, "cpuidle 10 s, per core powered off (asleep ms) / came "
			    "back at once / refused / too soon:");
			for (unsigned c = 0; c < 8; c++) {
				struct cpuidle_count *n = &redwood_cpuidle_counts[c], *l = &last[c];
				uint64_t ns;

				absolutetime_to_nanoseconds(n->slept_abs - l->slept_abs, &ns);
				any |= n->entered - l->entered;
				if (at >= sizeof(line)) {
					break;
				}
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu(%llu)/%llu/%llu/%llu", c, n->resumed - l->resumed,
				    ns / NSEC_PER_MSEC, n->returned - l->returned, n->refused - l->refused, n->skipped - l->skipped);
				*l = *n;
			}
			if (any) {
				extern void redwood_wake_report(void);

				dlog("%s", line);
				redwood_wake_report();
			}
			{
				extern uint32_t gic_resume_seen[][2];
				static uint8_t told;

				for (unsigned c = 0; c < 8; c++) {
					if ((gic_resume_seen[c][0] & (1u << 31)) && !(told & (1u << c))) {
						told |= 1u << c;
						dlog("cpuidle: core %u's first wake: its GIC had GICC_CTLR %#x PMR %#x ISENABLER0 %#x "
						    "(before they were set again)", c, (gic_resume_seen[c][0] >> 16) & 0x7fff,
						    gic_resume_seen[c][0] & 0xffff, gic_resume_seen[c][1]);
					}
				}
			}
		}
		report(hot ? "every 10 s (throttled)" : "every 10 s");
	}
}

void redwood_exynos_acpm_init(void);

void
redwood_exynos_acpm_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("dvfs", &mode, sizeof(mode)) || mode < 1 || mode > 2) {
		return;
	}
	if (kernel_thread_start(acpm_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
