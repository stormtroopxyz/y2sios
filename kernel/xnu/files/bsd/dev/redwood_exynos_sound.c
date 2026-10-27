/*
 * The S20+'s (y2s, Exynos 990) loudspeakers: what iOS plays (/dev/redwood-audio, redwood_audio.c) through the
 * SoC's audio block (ABOX) to the two Cirrus Logic CS35L41 amplifiers. Boot-arg snd=1.
 *
 * Android drives ABOX through its DSP (the "Calliope" Cortex-A32, which Samsung's firmware runs): the AP's driver
 * sends it the stream's buffer by IPC and the firmware programs the DMA. Here the DSP stays in reset and the AP
 * programs the hardware itself, with the same registers Samsung's kernel names (sound/soc/samsung/abox:
 * abox_soc_3.h, abox_if.c, abox_cmpnt.c, abox_dma.c, which writes these DMA registers itself for its dump DMA):
 *
 *   power       pd-aud: PMU AUD_CONFIGURATION 0x1900 bit 0, AUD_STATUS 0x1904 (flexpmu_cal_local_exynos9830.h
 *               aud_on), then the DTZPC restore of exynos-pd for need_smc 0x18e10204, then the audio pads out of
 *               retention (PMU 0x1920 bit 11, abox.c ABOX_PAD_NORMAL)
 *   clocks      CMU_AUD 0x18c00000: PLL_AUD0 1179.648 MHz (M 181 P 4 S 0 K 31740, pll_aud0_rate_table);
 *               AUDIF /16 = 73.728 MHz (abox.c: 384000 * 96 * 2 for uaif_max_div 512); UAIF2's bit clock
 *               AUDIF /24 = 3.072 MHz = 48 kHz * 2 slots * 32 bits; the bus /6
 *   SysMMU      0x18e50000 (ABOX's): left off, so that the DMA's addresses are physical (logged)
 *   the path    RDMA3 (16-bit stereo from our buffer) -> SPUS chain 3 (no ASRC) -> SIFS0, the mixer (32-bit
 *               stereo) -> UAIF2 (ROUTE_CTRL0) -> pins gpb1-0..3 (aud-i2s2-bus): DSP_A, NB_NF, ABOX the master,
 *               2 slots of 32 bits. Each DMA and the mixer need a slice of ABOX's staging SRAM (SBANK), which
 *               the firmware hands out: given here (RDMA3 at 0, the mixer at 512, 512 bytes each)
 *   amplifiers  two CS35L41 on a bit-banged I2C bus (gpp4-2 SCL, gpp4-3 SDA): 0x41 the earpiece/top speaker
 *               (left, slot 0, reset gpb2-4), 0x40 the bottom speaker (right, slot 1, reset gpb2-5). Set up as
 *               Cirrus's driver does (cs35l41.c: cs35l41_init, otp_unpack, apply_pdata for this phone's device
 *               tree, set_dai_fmt, hw_params, set_sysclk from SCLK, main_amp_event) with the PCM straight to the
 *               DAC (ASPRX1 -> DAC_PCM1_SRC): the amplifiers' own DSP (Cirrus's speaker protection) is not loaded;
 *               the samples go through redwood_guard.c instead, and the amplifiers are kept under the chip's
 *               full level (snd_below, eighths of a dB; default 18 dB) with iOS's volume on top, in their
 *               digital volume
 *
 * One thread does everything after the bring-up: keeps the DMA buffer filled ahead of where the DMA reads (from
 * what iOS wrote, else silence, through the guard), powers the amplifiers up when there is sound and down after
 * three seconds without, follows iOS's volume, and looks at the amplifiers' fault bits twice a second (a fault:
 * both off for the rest of the run).
 *
 * Boot-args: snd=1, snd_below=N (amplifiers N/8 dB under the chip's full level), snd_gain=N (the guard's gain,
 * eighths of a dB, at most 12 dB), snd_tone=MS (a 1 kHz tone at -20 dBFS for MS ms once the amplifiers are up: a
 * self-test without iOS).
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_bbi2c.h>
#include <sys/systm.h>
#include <vm/memory_types.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern void IOSleep(unsigned int milliseconds);
extern void loader_log_line(const char *line);
extern int redwood_wifi_dma_pool(uint64_t *phys, vm_offset_t *virt, size_t *size);

/* ---- addresses ------------------------------------------------------------------------------------------------ */

#define PMU_PHYS                0x15860000U
#define PMU_AUD_CONFIGURATION   0x1900U
#define PMU_AUD_STATUS          0x1904U
#define PMU_ABOX_PAD            0x1920U
#define PMU_ABOX_PAD_NORMAL     (1U << 11)
#define AUD_NEED_SMC            0x18e10204U
#define SMC_PREPARE_PD_ONOFF    0x82000410U

#define CMU_AUD_PHYS            0x18c00000U
#define PLL_LOCKTIME_AUD0       0x0000U
#define PLL_CON0_AUD0           0x0100U         /* bit 4: PLL (1) or OSC; bit 7 busy */
#define PLL_CON3_AUD0           0x010cU         /* ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0 */
#define PLL_CON5_AUD0           0x0114U         /* K 15:0 */
#define MUX_UAIF2               0x1014U
#define DIV_AUDIF               0x1800U
#define DIV_BUS                 0x1804U
#define DIV_BUSP                0x1808U
#define DIV_SCLK                0x1828U
#define DIV_UAIF2               0x1834U
#define CLK_BUSY                (1U << 16)
/* gates (cmucal-sfr.c, exynos9830): MANUAL 20 -> CG_VAL 21 decides; else ENABLE_AUTOMATIC_CLKGATING 28 (1: gated when idle) */
#define GATE_CG_VAL             (1U << 21)
#define GATE_MANUAL             (1U << 20)
#define GATE_AUTO               (1U << 28)
/* Q-channels: ENABLE 0 (1: the IP's handshake may stop the clock); with it 0, CLOCK_REQ 1 keeps the clock asked for */
#define QCH_ENABLE              (1U << 0)
#define QCH_CLOCK_REQ           (1U << 1)

#define SYSMMU_PHYS             0x18e50000U
#define MMU_CTRL                0x0000U
#define MMU_CFG                 0x0004U
#define MMU_STATUS              0x0008U
#define MMU_VERSION             0x0034U
#define MMU_CTRL_VM             0x8000U

#define ABOX_PHYS               0x18c50000U
#define ABOX_IP_INDEX           0x0000U
#define ABOX_VERSION            0x0004U
#define ABOX_SYSPOWER_CTRL      0x0010U
#define ABOX_SYSPOWER_STATUS    0x0014U
#define ABOX_DYN_CLOCK_OFF      0x0030U
#define ABOX_QCHANNEL_DISABLE   0x0038U         /* one bit per clock (abox.h enum qchannel): 1 ACLK, 2 + n BCLK_UAIFn */
#define QCH_ABOX_ACLK           1
#define QCH_ABOX_BCLK_UAIF(n)   (2 + (n))
#define ABOX_ROUTE_CTRL0        0x0040U         /* UAIFx's speaker source, 4 bits each: SIFSn + 1 */
#define ABOX_SPUS_CTRL_FC0      0x0200U         /* chains 0..3, 8 bits each: IN 6:5 (0 RDMA), OUT 4:1 (1 SIFS0), ASRC 0 */
#define ABOX_SPUS_CTRL1         0x0204U         /* MIXP_FORMAT 4:0 */
#define ABOX_SPUS_SBANK_RDMA(x) (0x0220U + 4 * (x))     /* SIZE 29:20, STR 13:4 */
#define ABOX_SPUS_SBANK_MIXP    0x0280U
#define ABOX_SIFS_CNT0          0x0290U
#define UAIF_ID                 2
#define ABOX_UAIF_CTRL0         (0x0500U + 0x10 * UAIF_ID)     /* MODE 2 (1 master), SPK_ENABLE 0 */
#define ABOX_UAIF_CTRL1         (0x0504U + 0x10 * UAIF_ID)
#define ABOX_UAIF_STATUS        (0x050cU + 0x10 * UAIF_ID)
#define RDMA_ID                 3
#define RDMA(off)               (0x1000U + 0x100 * RDMA_ID + (off))
#define RDMA_CTRL0              0x00U           /* BURST_LEN 22:19, FUNC 18:16, FORMAT 12:8, ENABLE 0 */
#define RDMA_BUF_STR            0x08U
#define RDMA_BUF_END            0x0cU
#define RDMA_BUF_OFFSET         0x10U
#define RDMA_STR_POINT          0x14U
#define RDMA_VOL_FACTOR         0x18U
#define RDMA_STATUS             0x30U           /* PROGRESS 31, RBUF_OFFSET 27:12 (16 bytes), RBUF_CNT 11:0 */

#define ABOX_FORMAT(width, channels) (((channels) - 1) | (((width) == 16 ? 1U : (width) == 24 ? 2U : 3U) << 3))

#define PINCTRL_PERIC1_GPB1     0x100U
#define PINCTRL_PERIC1_GPB2     0x120U
#define PINCTRL_PERIC0_GPP4     0x080U

/* ---- CS35L41 (cs35l41-registers.h) --------------------------------------------------------------------------- */

#define L41_DEVID               0x00000000U
#define L41_REVID               0x00000004U
#define L41_OTPID               0x00000010U
#define L41_TEST_KEY_CTL        0x00000040U
#define L41_OTP_MEM0            0x00000400U
#define L41_PWR_CTRL1           0x00002014U
#define L41_PWR_CTRL2           0x00002018U
#define L41_PLL_CLK_CTRL        0x00002c04U
#define L41_GLOBAL_CLK_CTRL     0x00002c0cU
#define L41_BSTCVRT_PEAK_CUR    0x00003808U
#define L41_SP_ENABLES          0x00004800U
#define L41_SP_RATE_CTRL        0x00004804U
#define L41_SP_FORMAT           0x00004808U
#define L41_SP_HIZ_CTRL         0x0000480cU
#define L41_SP_FRAME_RX_SLOT    0x00004820U
#define L41_SP_RX_WL            0x00004840U
#define L41_DAC_PCM1_SRC        0x00004c00U
#define L41_AMP_DIG_VOL_CTRL    0x00006000U     /* volume 13:3, 1/8 dB two's complement; 0x400 mute */
#define L41_AMP_GAIN_CTRL       0x00006c04U
#define L41_IRQ1_STATUS1        0x00010010U
#define L41_IRQ1_STATUS4        0x0001001cU
#define L41_IRQ1_MASK1          0x00010110U

#define L41_CHIP_ID             0x35a40U
#define L41R_CHIP_ID            0x35b40U
#define L41_OTP_BOOT_DONE       0x02U
#define L41_PUP_DONE            0x01000000U
#define L41_PDN_DONE            0x00800000U
#define L41_FAULTS              0x800281c0U     /* amp short, temperature, boost short / DCM UVP / OVP */
#define L41_VOL_MUTE            0x400U
#define L41_SCLK_3072000        0x21U           /* cs35l41_pll_sysclk */
#define L41_FS_48000            0x03U
#define L41_BST_IPK_4100MA      (((4100 - 1600) / 50) + 0x10)   /* cirrus,boost-peak-milliamp of the phone's tree */

/* cs35l41-tables.c otp_map_2 (OTP IDs 2 and 3); ID 1 differs only in element 93, which it skips */
static const struct { uint32_t reg; uint8_t shift, size; } otp_map[99] = {
	{0x02030,0,4}, {0x02030,7,1}, {0x0208c,24,6}, {0x02090,14,4}, {0x02090,10,4}, {0x0300c,11,4}, {0x0394c,23,2}, {0x03950,0,7},
	{0x03950,8,7}, {0x03950,16,8}, {0x03950,24,8}, {0x03954,0,7}, {0x03954,8,7}, {0x03954,16,8}, {0x03954,24,8}, {0x03958,0,7},
	{0x03958,8,7}, {0x03958,16,8}, {0x03958,24,8}, {0x0395c,0,7}, {0x0395c,8,7}, {0x0395c,16,8}, {0x0395c,24,8}, {0x0416c,0,8},
	{0x04160,0,7}, {0x0416c,8,8}, {0x04160,16,10}, {0x0416c,16,12}, {0x0416c,28,1}, {0x04170,0,6}, {0x04170,6,1}, {0x04170,8,6},
	{0x04170,14,1}, {0x04170,16,9}, {0x04360,0,5}, {0x04360,6,9}, {0x04448,0,8}, {0x04448,8,8}, {0x04448,16,8}, {0x04448,24,8},
	{0x0444c,0,3}, {0x06e30,0,5}, {0x06e30,8,5}, {0x06e30,16,5}, {0x06e30,24,5}, {0x06e34,0,5}, {0x06e34,8,5}, {0x06e34,16,5},
	{0x06e34,24,5}, {0x06e38,0,5}, {0x06e38,8,5}, {0x06e38,16,5}, {0x06e38,24,5}, {0x06e3c,0,5}, {0x06e3c,8,5}, {0x06e3c,16,5},
	{0x06e3c,24,5}, {0x06e40,0,5}, {0x06e40,8,5}, {0x06e40,16,5}, {0x06e40,24,5}, {0x06e44,0,5}, {0x06e48,0,10}, {0x06e48,10,10},
	{0x06e48,20,10}, {0x06e4c,0,10}, {0x06e4c,10,10}, {0x06e4c,20,10}, {0x06e50,0,10}, {0x06e50,10,10}, {0x06e50,20,10}, {0x06e54,0,10},
	{0x06e54,10,10}, {0x06e54,20,10}, {0x06e58,0,10}, {0x06e58,10,10}, {0x06e58,20,10}, {0x06e5c,0,10}, {0x06e5c,10,10}, {0x06e5c,20,10},
	{0x06e60,0,10}, {0x06e60,10,10}, {0x06e60,20,10}, {0x06e64,0,10}, {0x07418,7,5}, {0x0741c,0,5}, {0x0741c,11,4}, {0x0741c,19,4},
	{0x07434,17,1}, {0x07434,18,7}, {0x07068,0,9}, {0x0410c,7,1}, {0x0400c,0,7}, {0x04000,11,1}, {0x17040,0,8}, {0x17040,8,8},
	{0x17040,16,8}, {0x17040,24,8}, {0x17044,0,24},
};
#define OTP_ID1_SKIP            93

/* ---- state ---------------------------------------------------------------------------------------------------- */

#define BUF_BYTES               32768U          /* the DMA's ring: 8192 frames, 171 ms */
#define BUF_FRAMES              (BUF_BYTES / 4)
#define PERIOD_BYTES            4096U
#define LEAD_FRAMES             2880U           /* written this far ahead of the DMA: 60 ms */
#define CHUNK                   1024U           /* the guard's most at once */
#define RING_FRAMES             16384U          /* iOS's samples waiting: 341 ms; a power of two */
#define SILENCE                 8
#define QUIET_OFF_US            3000000ull

static volatile uint8_t *pmu, *cmu, *mmu, *abox;
static volatile uint8_t *gpb1, *gpb2;
static uint64_t buf_phys;
static int16_t *buf;
static bool started, amps_ok, amps_on, faulted, use_status;
static uint32_t snd_below = 144, snd_gain = 12 * 8, snd_tone;

struct amp {
	const char *name;
	uint8_t addr;
	uint32_t reset_pin;
	uint32_t slot;
	bool present;
};
static struct amp amps[2] = {
	{ "left (top)", 0x41, 4, 0, false },
	{ "right (bottom)", 0x40, 5, 1, false },
};
static struct redwood_bbi2c amp_bus = { .name = "amp bus gpp4-2/3", .scl = 2, .sda = 3, .half_us = 3 };

static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_head, ring_tail;
static volatile uint64_t last_sound_us;
/*
 * Quiet: the amplifiers off, no tone, no sound from iOS for QUIET_OFF_US (it keeps sending silence): the DMA's
 * whole ring is silence (it goes on reading it round and round) and the thread sleeps until iOS sends sound
 * (exynos_sink), a second at most, the silence meanwhile dropped -- instead
 * of feeding silence every 4 ms (some 1000 wake-ups a second on the big cores: the cpuidle wake record,
 * 27 October).
 */
static volatile bool snd_quiet;
static uint64_t quiet_spells, quiet_us;
static uint64_t ios_frames, fed_frames, silent_frames, underruns;

static const int16_t tone[48] = {
	0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3028, 3165, 3249, 3277, 3249, 3165, 3028, 2838, 2600, 2317, 1995, 1638,
	1254, 848, 428, 0, -428, -848, -1254, -1638, -1995, -2317, -2600, -2838, -3028, -3165, -3249, -3277, -3249, -3165, -3028,
	-2838, -2600, -2317, -1995, -1639, -1254, -848, -428,
};

static void snd_log(const char *fmt, ...) __printflike(1, 2);
static void
snd_log(const char *fmt, ...)
{
	char line[320];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	printf("%s", line);
	loader_log_line(line);
}

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

/*
 * The speaker path's clocks through CMU_AUD's gates and Q-channels, opened as Samsung's cal-if does it
 * (ra.c: ra_set_gate pass, ra_req_enable_qch). Without the DSP's driver nothing asks for them, and an
 * automatically gated BCLK_UAIF2 is a UAIF that never pulls a sample: RDMA3 stayed "in progress" with 0
 * frames read and UAIF2's status 0 (PLANS/08-audio.md). Each register is said before and after.
 */
static void
snd_clocks_on(void)
{
	static const struct { uint32_t off; const char *name; } gates[] = {
		{ 0x2010, "ABOX ACLK" }, { 0x2018, "ABOX BCLK_CNT" }, { 0x2028, "ABOX BCLK_UAIF2" },
		{ 0x2048, "ABOX SCLK" }, { 0x2058, "LHM_AXI_P" }, { 0x205c, "LHS_AXI_D" },
		{ 0x2070, "RSTNSYNC BUSP" }, { 0x2074, "RSTNSYNC BUS" }, { 0x2078, "RSTNSYNC CNT" },
		{ 0x2094, "RSTNSYNC SCLK" }, { 0x20a0, "RSTNSYNC UAIF2" },
	};
	static const struct { uint32_t off; const char *name; } qchs[] = {
		{ 0x301c, "QCH ACLK" }, { 0x3028, "QCH BCLK2" }, { 0x3044, "QCH CNT" }, { 0x3048, "QCH SCLK" },
	};
	char line[600];
	size_t at = 0;

	at += snprintf(line + at, sizeof(line) - at, "snd: CMU_AUD gates before -> after:");
	for (unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) {
		uint32_t was = rd(cmu, gates[i].off), now = was;

		if (was & GATE_MANUAL) {
			now |= GATE_CG_VAL;
		} else {
			now &= ~GATE_AUTO;
		}
		if (now != was) {
			wr(cmu, gates[i].off, now);
		}
		at += snprintf(line + at, sizeof(line) - at, " %s %#x->%#x;", gates[i].name, was, rd(cmu, gates[i].off));
	}
	snd_log("%s\n", line);
	at = 0;
	at += snprintf(line + at, sizeof(line) - at, "snd: CMU_AUD Q-channels before -> after:");
	for (unsigned i = 0; i < sizeof(qchs) / sizeof(qchs[0]); i++) {
		uint32_t was = rd(cmu, qchs[i].off);

		wr(cmu, qchs[i].off, (was & ~QCH_ENABLE) | QCH_CLOCK_REQ);
		at += snprintf(line + at, sizeof(line) - at, " %s %#x->%#x;", qchs[i].name, was, rd(cmu, qchs[i].off));
	}
	snd_log("%s\n", line);
}

static inline void
upd(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t value)
{
	wr(base, off, (rd(base, off) & ~mask) | (value & mask));
}

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

static bool
wait_bits(volatile uint8_t *base, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t i = 0; i < us; i += 10) {
		if ((rd(base, off) & mask) == want) {
			return true;
		}
		delay_for_interval(10, NSEC_PER_USEC);
	}
	return (rd(base, off) & mask) == want;
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

/* ---- the amplifiers ------------------------------------------------------------------------------------------- */

static bool
amp_write(struct amp *a, uint32_t reg, uint32_t value)
{
	uint8_t b[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		         (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };

	return redwood_bbi2c_xfer(&amp_bus, a->addr, b, sizeof(b), NULL, 0);
}

static bool
amp_read(struct amp *a, uint32_t reg, uint32_t *value)
{
	uint8_t r[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg }, v[4] = { 0 };
	bool ok = redwood_bbi2c_xfer(&amp_bus, a->addr, r, sizeof(r), v, sizeof(v));

	*value = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
	return ok;
}

static bool
amp_update(struct amp *a, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t old;

	return amp_read(a, reg, &old) && amp_write(a, reg, (old & ~mask) | (value & mask));
}

static bool
amp_unlock(struct amp *a, bool unlock)
{
	return unlock ? amp_write(a, L41_TEST_KEY_CTL, 0x5555) && amp_write(a, L41_TEST_KEY_CTL, 0xaaaa) :
	       amp_write(a, L41_TEST_KEY_CTL, 0xcccc) && amp_write(a, L41_TEST_KEY_CTL, 0x3333);
}

/* cs35l41_otp_unpack: the chip's trims, from its OTP into the registers they belong in */
static bool
amp_otp_unpack(struct amp *a)
{
	uint32_t mem[32], id = 0, bit = 16, word = 2;

	if (!amp_unlock(a, true) || !amp_read(a, L41_OTPID, &id)) {
		return false;
	}
	for (unsigned i = 0; i < 32; i++) {
		if (!amp_read(a, L41_OTP_MEM0 + 4 * i, &mem[i])) {
			return false;
		}
	}
	id &= 0xf;
	if ((mem[1] & 0xfffffffe) != 0x1500 || (mem[2] & 0xffff) != 0x300 || id < 1 || id > 3) {
		snd_log("snd: amp %s: OTP header %#x %#x, id %u: not unpacked\n", a->name, mem[1], mem[2], id);
		amp_unlock(a, false);
		return false;
	}
	for (unsigned i = 0; i < 99; i++) {
		uint32_t size = otp_map[i].size, value;

		if (bit + size - 1 >= 32) {
			value = (mem[word] >> bit) & ((1U << (32 - bit)) - 1);
			word++;
			value |= (mem[word] & ((1U << (bit + size - 32)) - 1)) << (32 - bit);
			bit += size - 32;
		} else {
			value = (mem[word] >> bit) & (size == 32 ? ~0U : (1U << size) - 1);
			bit += size;
		}
		if (bit == 32) {
			bit = 0;
			word++;
		}
		if (otp_map[i].reg != 0 && !(id == 1 && i == OTP_ID1_SKIP)) {
			uint32_t mask = (size == 32 ? ~0U : (1U << size) - 1) << otp_map[i].shift;

			if (!amp_update(a, otp_map[i].reg, mask, value << otp_map[i].shift)) {
				amp_unlock(a, false);
				return false;
			}
		}
	}
	return amp_unlock(a, false);
}

static const char *
amp_bad(struct amp *a, const char *what)
{
	snd_log("snd: amp %s (%#x): %s\n", a->name, a->addr, what);
	a->present = false;
	return NULL;
}

/* reset, identified, trimmed and set up for our stream; powered down (GLOBAL_EN 0) and muted */
static bool
amp_prepare(struct amp *a)
{
	uint32_t devid = 0, revid = 0, status = 0, fmt = 0, slot = 0, vol = 0, gain = 0, pwr2 = 0, pll = 0;
	unsigned i;

	if (!amp_read(a, L41_DEVID, &devid) || !amp_read(a, L41_REVID, &revid)) {
		amp_bad(a, "does not answer");
		return false;
	}
	if (devid != ((revid & 0xf) % 2 ? L41R_CHIP_ID : L41_CHIP_ID)) {
		snd_log("snd: amp %s: device ID %#x, revision %#x: not a CS35L41\n", a->name, devid, revid);
		a->present = false;
		return false;
	}
	amp_write(a, L41_IRQ1_MASK1, 0x7ffcfe3f);
	if (revid == 0xa0) {
		amp_unlock(a, true);
		amp_write(a, 0x3854, 0x05180240);
		amp_write(a, 0x7418, 0x9091a1c8);
		amp_write(a, 0x3014, 0x0200ee0e);
		amp_write(a, 0x381c, 0x00000051);
		amp_write(a, 0x0054, 0x00000004);
		amp_write(a, 0x10318, 0);
		amp_write(a, 0x10b18, 0);
		amp_unlock(a, false);
	}
	for (i = 0; i < 100; i++) {
		if (amp_read(a, L41_IRQ1_STATUS4, &status) && (status & L41_OTP_BOOT_DONE)) {
			break;
		}
		IOSleep(1);
	}
	if (!(status & L41_OTP_BOOT_DONE)) {
		amp_bad(a, "OTP_BOOT_DONE never came");
		return false;
	}
	if (!amp_otp_unpack(a)) {
		amp_bad(a, "the OTP trims could not be unpacked");
		return false;
	}
	/* the phone's tree (apply_pdata): ASP SDOUT hi-z when unused, the boost's peak current */
	amp_update(a, L41_SP_HIZ_CTRL, 0x3, 0x3);
	amp_update(a, L41_BSTCVRT_PEAK_CUR, 0x7f, L41_BST_IPK_4100MA);
	/* set_dai_fmt DSP_A | NB_NF | CBS_CFS: slave, ASP_FMT 0, no inversions; hw_params: 48 kHz, slots of 32, 24 bits */
	amp_update(a, L41_SP_FORMAT, 0xff000000 | 0x0700 | 0x40 | 0x10 | 0x04 | 0x01, 32U << 24);
	amp_update(a, L41_SP_RX_WL, 0x3f, 24);
	amp_update(a, L41_SP_FRAME_RX_SLOT, 0x3f3f, a->slot | (a->slot ^ 1) << 8);
	amp_update(a, L41_GLOBAL_CLK_CTRL, 0x1f, L41_FS_48000);
	/* set_sysclk from SCLK at 3.072 MHz */
	amp_update(a, L41_SP_RATE_CTRL, 0x3f, L41_SCLK_3072000);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0800, 0x0800);
	amp_update(a, L41_PLL_CLK_CTRL, 0x07e0, L41_SCLK_3072000 << 5);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0010, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0007, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0800, 0);
	amp_update(a, L41_PLL_CLK_CTRL, 0x0010, 0x0010);
	/* the PCM to the DAC, muted; ASPRX1 on */
	amp_write(a, L41_DAC_PCM1_SRC, 0x08);
	amp_update(a, L41_AMP_DIG_VOL_CTRL, 0x7ff << 3, L41_VOL_MUTE << 3);
	amp_update(a, L41_SP_ENABLES, 1U << 16, 1U << 16);

	amp_read(a, L41_SP_FORMAT, &fmt);
	amp_read(a, L41_SP_FRAME_RX_SLOT, &slot);
	amp_read(a, L41_AMP_DIG_VOL_CTRL, &vol);
	amp_read(a, L41_AMP_GAIN_CTRL, &gain);
	amp_read(a, L41_PWR_CTRL2, &pwr2);
	amp_read(a, L41_PLL_CLK_CTRL, &pll);
	snd_log("snd: amp %s (%#x): CS35L41 %#x rev %#x, trimmed; SP_FORMAT %#x RX_SLOT %#x PLL %#x VOL %#x GAIN %#x PWR2 %#x\n",
	    a->name, a->addr, devid, revid, fmt, slot, pll, vol, gain, pwr2);
	a->present = true;
	return true;
}

static uint32_t amps_volume_reg = L41_VOL_MUTE;        /* as amp_prepare() leaves them */

static void
amps_volume(uint32_t below, bool muted)
{
	uint32_t reg = (muted || below >= 816) ? L41_VOL_MUTE : (uint32_t)(-(int32_t)below) & 0x7ff;

	if (reg == amps_volume_reg) {
		return;
	}
	amps_volume_reg = reg;
	for (unsigned i = 0; i < 2; i++) {
		if (amps[i].present) {
			amp_update(&amps[i], L41_AMP_DIG_VOL_CTRL, 0x7ff << 3, reg << 3);
		}
	}
}

/* main_amp_event POST_PMU: the power-up patch, GLOBAL_EN, PUP_DONE */
static bool
amps_up(void)
{
	bool ok = true;

	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t status = 0;
		unsigned t;

		if (!a->present) {
			continue;
		}
		amp_write(a, L41_TEST_KEY_CTL, 0x55);
		amp_write(a, L41_TEST_KEY_CTL, 0xaa);
		amp_write(a, 0x2084, 0x002f1aa0);
		amp_write(a, L41_TEST_KEY_CTL, 0xcc);
		amp_write(a, L41_TEST_KEY_CTL, 0x33);
		amp_update(a, L41_PWR_CTRL1, 1, 1);
		for (t = 0; t < 20; t++) {
			if (amp_read(a, L41_IRQ1_STATUS1, &status) && (status & L41_PUP_DONE)) {
				break;
			}
			IOSleep(1);
		}
		amp_write(a, L41_IRQ1_STATUS1, L41_PUP_DONE);
		if (!(status & L41_PUP_DONE) || (status & L41_FAULTS)) {
			snd_log("snd: amp %s: power-up %s (IRQ1_STATUS1 %#x)\n", a->name,
			    (status & L41_FAULTS) ? "FAULT" : "not done", status);
			ok = false;
		}
	}
	return ok;
}

static void
amps_down(void)
{
	amps_volume(0, true);
	for (unsigned i = 0; i < 2; i++) {
		struct amp *a = &amps[i];
		uint32_t status = 0;

		if (!a->present) {
			continue;
		}
		amp_update(a, L41_PWR_CTRL1, 1, 0);
		for (unsigned t = 0; t < 20; t++) {
			if (amp_read(a, L41_IRQ1_STATUS1, &status) && (status & L41_PDN_DONE)) {
				break;
			}
			IOSleep(1);
		}
		amp_write(a, L41_IRQ1_STATUS1, L41_PDN_DONE);
		amp_write(a, L41_TEST_KEY_CTL, 0x55);
		amp_write(a, L41_TEST_KEY_CTL, 0xaa);
		amp_write(a, 0x2084, 0x002f1aa3);
		amp_write(a, L41_TEST_KEY_CTL, 0xcc);
		amp_write(a, L41_TEST_KEY_CTL, 0x33);
	}
}

static uint32_t
amps_faults(void)
{
	uint32_t faults = 0;

	for (unsigned i = 0; i < 2; i++) {
		uint32_t status = 0;

		if (amps[i].present && amp_read(&amps[i], L41_IRQ1_STATUS1, &status)) {
			faults |= status & L41_FAULTS;
		}
	}
	return faults;
}

static bool
amps_bring_up(void)
{
	char report[200];
	bool any = false;

	gpb2 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB2);
	amp_bus.bank = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC0, PINCTRL_PERIC0_GPP4);
	for (unsigned i = 0; i < 2; i++) {
		redwood_gpio_set(gpb2, amps[i].reset_pin, 0);
	}
	IOSleep(2);
	for (unsigned i = 0; i < 2; i++) {
		redwood_gpio_set(gpb2, amps[i].reset_pin, 1);
	}
	IOSleep(3);
	if (!redwood_bbi2c_setup(&amp_bus, true, report, sizeof(report))) {
		snd_log("snd: %s: the bus is not free\n", report);
		return false;
	}
	for (unsigned i = 0; i < 2; i++) {
		any |= amp_prepare(&amps[i]);
	}
	return any;
}

/* ---- ABOX ----------------------------------------------------------------------------------------------------- */

static bool
aud_power_on(void)
{
	bool was_on = (rd(pmu, PMU_AUD_STATUS) & 1) != 0;
	uint64_t ret;

	if (!was_on) {
		upd(pmu, PMU_AUD_CONFIGURATION, 1, 1);
		if (!wait_bits(pmu, PMU_AUD_STATUS, 1, 1, 10000)) {
			snd_log("snd: pd-aud does not power on (status %#x)\n", rd(pmu, PMU_AUD_STATUS));
			return false;
		}
	}
	ret = smc4(SMC_PREPARE_PD_ONOFF, 1, AUD_NEED_SMC, 2);
	upd(pmu, PMU_ABOX_PAD, PMU_ABOX_PAD_NORMAL, PMU_ABOX_PAD_NORMAL);
	snd_log("snd: pd-aud %s; DTZPC restore answers %#llx; pads %#x\n", was_on ? "was on" : "powered on", ret,
	    rd(pmu, PMU_ABOX_PAD));
	return true;
}

static void
set_div(uint32_t off, uint32_t mask, uint32_t value)
{
	upd(cmu, off, mask, value);
	wait_bits(cmu, off, CLK_BUSY, 0, 1000);
}

static bool
aud_clocks(void)
{
	uint32_t con3;
	uint32_t found_con3 = rd(cmu, PLL_CON3_AUD0);
	uint32_t found_con5 = rd(cmu, PLL_CON5_AUD0);
	uint32_t fp = (found_con3 >> 8) & 0x3fU, fs = found_con3 & 7U;
	uint64_t fm = (found_con3 >> 16) & 0x3ffU, fk = found_con5 & 0xffffU;
	bool locked = (found_con3 & (1U << 29)) != 0;
	/*
	 * The rate, not the fields: S-Boot locks the same 1179.648 MHz with a different decomposition
	 * (M 45 P 1 K 24319 where this driver writes M 181 P 4 K 31740), so comparing M/P/K called a
	 * working PLL "wrong" and reset it.
	 */
	uint64_t found_rate = (fp == 0 || fs > 6) ? 0 :
	    (26000000ULL * ((fm << 16) + fk)) / ((uint64_t)fp << (16 + fs));
	bool ours = locked && found_rate > 1179648000ULL - 118000ULL && found_rate < 1179648000ULL + 118000ULL;

	snd_log("snd: CMU_AUD as found: LOCKTIME %#x CON0 %#x CON1 %#x CON2 %#x CON3 %#x CON4 %#x CON5 %#x (%s%s)\n",
	    rd(cmu, PLL_LOCKTIME_AUD0), rd(cmu, PLL_CON0_AUD0), rd(cmu, 0x104), rd(cmu, 0x108),
	    found_con3, rd(cmu, 0x110), found_con5, locked ? "locked" : "not locked",
	    ours ? ", the rate we want" : "");

	/*
	 * S-Boot plays its own sounds, so it may have left the audio PLL running. Keep it when it is
	 * locked at the rate this driver wants: resetting a working PLL is what failed before.
	 */
	if (locked && ours) {
		snd_log("snd: PLL_AUD0 was already locked at our rate: leaving it alone\n");
	} else {
		/* PLL_AUD0 off its output while it is set (ra_enable_pll / ra_pll_set_pmsk) */
		upd(cmu, PLL_CON0_AUD0, 1U << 4, 0);
		wait_bits(cmu, PLL_CON0_AUD0, 1U << 7, 0, 1000);
		upd(cmu, PLL_CON3_AUD0, 1U << 31, 0);
		wr(cmu, PLL_LOCKTIME_AUD0, 4 * 500);
		upd(cmu, PLL_CON5_AUD0, 0xffff, 31740);
		con3 = rd(cmu, PLL_CON3_AUD0) & ~((0x3ffU << 16) | (0x3fU << 8) | 7U);
		wr(cmu, PLL_CON3_AUD0, con3 | 181U << 16 | 4U << 8 | 0U | 1U << 4 | 1U << 31);
		if (!wait_bits(cmu, PLL_CON3_AUD0, 1U << 29, 1U << 29, 10000)) {
			snd_log("snd: PLL_AUD0 does not lock: CON0 %#x CON1 %#x CON2 %#x CON3 %#x CON4 %#x CON5 %#x\n",
			    rd(cmu, PLL_CON0_AUD0), rd(cmu, 0x104), rd(cmu, 0x108), rd(cmu, PLL_CON3_AUD0),
			    rd(cmu, 0x110), rd(cmu, PLL_CON5_AUD0));
			return false;
		}
		upd(cmu, PLL_CON0_AUD0, 1U << 4, 1U << 4);
		wait_bits(cmu, PLL_CON0_AUD0, 1U << 7, 0, 1000);
	}

	set_div(DIV_BUS, 0x7, 5);               /* 196.6 MHz */
	set_div(DIV_BUSP, 0x3, 2);              /* 65.5 MHz */
	set_div(DIV_AUDIF, 0x1ff, 15);          /* 73.728 MHz */
	set_div(DIV_SCLK, 0x1ff, 0);            /* ABOX's SCLK: 73.728 MHz */
	snd_clocks_on();
	upd(cmu, MUX_UAIF2, 0x3, 0);            /* UAIF2 from AUDIF */
	wait_bits(cmu, MUX_UAIF2, CLK_BUSY, 0, 1000);
	set_div(DIV_UAIF2, 0x1ff, 23);          /* 3.072 MHz */
	snd_log("snd: CMU_AUD: PLL_AUD0 CON0 %#x CON3 %#x CON5 %#x; BUS %#x BUSP %#x AUDIF %#x SCLK %#x UAIF2 mux %#x div %#x\n",
	    rd(cmu, PLL_CON0_AUD0), rd(cmu, PLL_CON3_AUD0), rd(cmu, PLL_CON5_AUD0), rd(cmu, DIV_BUS), rd(cmu, DIV_BUSP),
	    rd(cmu, DIV_AUDIF), rd(cmu, DIV_SCLK), rd(cmu, MUX_UAIF2), rd(cmu, DIV_UAIF2));
	return true;
}

static void
abox_route(void)
{
	uint32_t ctrl0, ctrl1;

	snd_log("snd: ABOX IP %#x version %#x; SysMMU CTRL %#x CTRL_VM %#x CFG %#x STATUS %#x VERSION %#x\n",
	    rd(abox, ABOX_IP_INDEX), rd(abox, ABOX_VERSION), rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM), rd(mmu, MMU_CFG),
	    rd(mmu, MMU_STATUS), rd(mmu, MMU_VERSION));
	if ((rd(mmu, MMU_CTRL) & 1) || (rd(mmu, MMU_CTRL_VM) & 1)) {
		wr(mmu, MMU_CTRL_VM, 0);
		wr(mmu, MMU_CTRL, 0);
		snd_log("snd: SysMMU was on: off (CTRL %#x CTRL_VM %#x)\n", rd(mmu, MMU_CTRL), rd(mmu, MMU_CTRL_VM));
	}

	wr(abox, ABOX_SYSPOWER_CTRL, 1);        /* abox_request_dram_on */
	wait_bits(abox, ABOX_SYSPOWER_STATUS, 1, 1, 10000);

	/* RDMA3 -> chain 3 -> SIFS0 (the mixer, 32-bit stereo) -> UAIF2 */
	upd(abox, ABOX_SPUS_CTRL_FC0, 0xffU << (8 * RDMA_ID), 0x02U << (8 * RDMA_ID));
	upd(abox, ABOX_SPUS_CTRL1, 0x1f, ABOX_FORMAT(32, 2));
	if (rd(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID)) == 0) {
		wr(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID), (512U << 16) | 0x000);
	}
	if (rd(abox, ABOX_SPUS_SBANK_MIXP) == 0) {
		wr(abox, ABOX_SPUS_SBANK_MIXP, (512U << 16) | 0x200);
	}
	wr(abox, ABOX_SIFS_CNT0, 0);
	upd(abox, ABOX_ROUTE_CTRL0, 0xfU << (4 * UAIF_ID), 1U << (4 * UAIF_ID));

	/*
	 * UAIF2's bit clock past the ABOX's own Q-channel: Samsung's driver disables it when the interface's BCLK
	 * widget powers up (abox_if.c, abox_if_bclk_event -> abox_disable_qchannel(ABOX_BCLK_UAIF0 + id, 1)), and
	 * with nothing asking for the clock the UAIF never pulls a sample -- RDMA3 "in progress", 0 frames read,
	 * with CMU_AUD's gates and Q-channels all open (run 30). The bus clock (ACLK) as well.
	 */
	{
		uint32_t qch = rd(abox, ABOX_QCHANNEL_DISABLE), dyn = rd(abox, ABOX_DYN_CLOCK_OFF);

		wr(abox, ABOX_QCHANNEL_DISABLE, qch | (1U << QCH_ABOX_BCLK_UAIF(UAIF_ID)) | (1U << QCH_ABOX_ACLK));
		snd_log("snd: ABOX Q-channels disabled %#x -> %#x (BCLK_UAIF%u, ACLK); dynamic clock off %#x\n", qch,
		    rd(abox, ABOX_QCHANNEL_DISABLE), UAIF_ID, dyn);
	}

	/* UAIF2: set_fmt DSP_A | NB_NF | CBS_CFS, hw_params 32 bits x 2 */
	ctrl0 = rd(abox, ABOX_UAIF_CTRL0);
	/*
	 * bit 2 = MODE (1: master, it makes the BCLK), bit 0 = SPK_ENABLE: Samsung sets the latter
	 * from abox_uaif_mute_stream on unmute, and with it clear the mixer has no consumer, so the
	 * RDMA never advances (it sat at 0 with the enable bit set).
	 */
	ctrl0 = (ctrl0 & ~(1U << 2 | 1U)) | 1U << 2 | 1U;
	wr(abox, ABOX_UAIF_CTRL0, ctrl0);
	ctrl1 = ABOX_FORMAT(32, 2) << 24 | 1U << 23 | 1U << 22 | 0U << 21 | (2U - 1) << 18 | 31U << 12 | 31U << 6 | 31U;
	wr(abox, ABOX_UAIF_CTRL1, ctrl1);

	gpb1 = redwood_gpio_bank(REDWOOD_PINCTRL_PERIC1, PINCTRL_PERIC1_GPB1);
	for (uint32_t pin = 0; pin < 4; pin++) {
		redwood_gpio_pull(gpb1, pin, REDWOOD_GPIO_PULL_NONE);
		redwood_gpio_func(gpb1, pin, 2);
	}

	/* RDMA3: our ring, 16-bit stereo */
	wr(abox, RDMA(RDMA_CTRL0), ABOX_FORMAT(16, 2) << 8 | 4U << 19);
	wr(abox, RDMA(RDMA_BUF_STR), (uint32_t)buf_phys);
	wr(abox, RDMA(RDMA_BUF_END), (uint32_t)buf_phys + BUF_BYTES);
	wr(abox, RDMA(RDMA_BUF_OFFSET), PERIOD_BYTES);
	wr(abox, RDMA(RDMA_STR_POINT), (uint32_t)buf_phys);
	if ((rd(abox, RDMA(RDMA_VOL_FACTOR)) & 0xffffff) == 0) {
		wr(abox, RDMA(RDMA_VOL_FACTOR), 0x800000);
	}
	/*
	 * Start it: CTRL0 bit 0 is ABOX_RDMA_ENABLE (abox_soc_2.h), and Samsung's PCM trigger
	 * normally sets it from the DSP's mailbox. Written last, so the DMA never runs over a
	 * buffer that is not set up yet.
	 */
	upd(abox, RDMA(RDMA_CTRL0), 1U, 1U);
	snd_log("snd: RDMA3 enabled: CTRL0 %#x STATUS %#x\n", rd(abox, RDMA(RDMA_CTRL0)),
	    rd(abox, RDMA(RDMA_STATUS)));
	snd_log("snd: ABOX: SYSPOWER %#x/%#x ROUTE0 %#x FC0 %#x SPUS1 %#x SBANK %#x/%#x UAIF2 %#x %#x RDMA3 %#x %#x-%#x vol %#x\n",
	    rd(abox, ABOX_SYSPOWER_CTRL), rd(abox, ABOX_SYSPOWER_STATUS), rd(abox, ABOX_ROUTE_CTRL0),
	    rd(abox, ABOX_SPUS_CTRL_FC0), rd(abox, ABOX_SPUS_CTRL1), rd(abox, ABOX_SPUS_SBANK_RDMA(RDMA_ID)),
	    rd(abox, ABOX_SPUS_SBANK_MIXP), rd(abox, ABOX_UAIF_CTRL0), rd(abox, ABOX_UAIF_CTRL1), rd(abox, RDMA(RDMA_CTRL0)),
	    rd(abox, RDMA(RDMA_BUF_STR)), rd(abox, RDMA(RDMA_BUF_END)), rd(abox, RDMA(RDMA_VOL_FACTOR)));
}

/* where the DMA reads, in frames into the ring (abox_rdma_pointer) */
static uint32_t
dma_position(uint32_t *raw)
{
	uint32_t st = rd(abox, RDMA(RDMA_STATUS)), bytes = (((st >> 12) & 0xffff) << 4) + (st & 0xfff);

	if (raw != NULL) {
		*raw = st;
	}
	return (bytes % BUF_BYTES) / 4;
}

static void
abox_start(void)
{
	uint32_t raw[8], pos[8];
	uint64_t t0, t[8];

	bzero(buf, BUF_BYTES);
	__asm__ volatile ("dsb sy" ::: "memory");
	upd(abox, ABOX_UAIF_CTRL0, 1, 1);
	upd(abox, RDMA(RDMA_CTRL0), 1, 1);
	t0 = now_us();
	for (unsigned i = 0; i < 8; i++) {
		IOSleep(5);
		pos[i] = dma_position(&raw[i]);
		t[i] = now_us() - t0;
	}
	snd_log("snd: started; RDMA3 STATUS %#x@%llu %#x@%llu %#x@%llu %#x@%llu ... %#x@%llu us (frames %u %u %u %u .. %u); "
	    "UAIF2 STATUS %#x\n", raw[0], t[0], raw[1], t[1], raw[2], t[2], raw[3], t[3], raw[7], t[7], pos[0], pos[1], pos[2],
	    pos[3], pos[7], rd(abox, ABOX_UAIF_STATUS));
}

/* ---- iOS's samples -------------------------------------------------------------------------------------------- */

/* from the writer's thread, under redwood_audio.c's lock: never waits */
static uint32_t
exynos_sink(const int16_t *frames, uint32_t count)
{
	uint32_t head = ring_head, room = RING_FRAMES - (head - ring_tail), taken = count < room ? count : room;
	bool sound = false;

	for (uint32_t i = 0; i < taken; i++) {
		int16_t l = frames[2 * i], r = frames[2 * i + 1];

		ring[2 * ((head + i) % RING_FRAMES)] = l;
		ring[2 * ((head + i) % RING_FRAMES) + 1] = r;
		sound |= l > SILENCE || l < -SILENCE || r > SILENCE || r < -SILENCE;
	}
	OSMemoryBarrier();
	ring_head = head + taken;
	ios_frames += taken;
	if (sound) {
		last_sound_us = now_us();
	}
	if (sound && snd_quiet) {
		thread_wakeup((event_t)&ring_head);     /* the thread is waiting for something to play */
	}
	return taken;
}

static uint32_t tone_left, tone_phase;

/* `n' frames (at most CHUNK) at frame `at' of the DMA's ring: from iOS (or the tone), the rest silence, through the guard */
static void
fill(uint64_t at, uint32_t n)
{
	static int16_t chunk[CHUNK * 2];
	uint32_t tail = ring_tail, have = ring_head - tail, take = have < n ? have : n, i = 0;

	if (tone_left != 0) {
		for (; i < n && tone_left != 0; i++, tone_left--) {
			chunk[2 * i] = chunk[2 * i + 1] = tone[tone_phase];
			tone_phase = (tone_phase + 1) % 48;
		}
		take = 0;
	} else {
		OSMemoryBarrier();
		for (; i < take; i++) {
			chunk[2 * i] = ring[2 * ((tail + i) % RING_FRAMES)];
			chunk[2 * i + 1] = ring[2 * ((tail + i) % RING_FRAMES) + 1];
		}
		OSMemoryBarrier();
		ring_tail = tail + take;
	}
	fed_frames += i;
	silent_frames += n - i;
	bzero(chunk + 2 * i, (n - i) * 4);
	redwood_guard_process(chunk, n);
	if (take != 0 && redwood_audio_recording_fed()) {
		redwood_audio_record_fed(chunk, take);
	}
	memcpy(buf + 2 * (at % BUF_FRAMES), chunk, n * 4);
}

/* ---- the thread ----------------------------------------------------------------------------------------------- */

static void
sound_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint64_t written, consumed, base, t0, last_look = 0, last_vol = 0, checked_at = 0;
	uint64_t status_consumed = 0, quiet_since = 0;
	uint32_t last_pos, generation = 0, below = 0;
	bool muted = false, decided = false;

	if (!aud_power_on() || !aud_clocks()) {
		return;
	}
	abox_route();
	amps_ok = amps_bring_up();      /* (their setup needs no clock; their power-up does) */
	abox_start();
	/* frames are counted on from where the DMA is now, in the ring's terms: ring index = count % BUF_FRAMES */
	t0 = now_us();
	last_pos = dma_position(NULL);
	base = written = last_pos;
	redwood_guard_configure(true, snd_gain);
	redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
	redwood_audio_set_sink("the S20+'s speakers (ABOX UAIF2, CS35L41 x2)", exynos_sink);
	if (snd_tone != 0) {
		tone_left = snd_tone * 48;
		last_sound_us = now_us();
	}
	snd_log("snd: running (amplifiers %s, %u/8 dB under the chip's full level, guard gain %u/8 dB%s)\n",
	    amps_ok ? "ready" : "MISSING", snd_below, snd_gain, snd_tone ? ", the test tone first" : "");

	for (;;) {
		uint64_t now = now_us(), by_time = (now - t0) * REDWOOD_AUDIO_RATE / 1000000;
		uint32_t pos = dma_position(NULL), delta = (pos + BUF_FRAMES - last_pos) % BUF_FRAMES;

		last_pos = pos;
		status_consumed += delta;
		/* after a quarter second: does the DMA's own count agree with the clock? (it should, within a period) */
		if (!decided && now - t0 >= 250000) {
			int64_t diff = (int64_t)status_consumed - (int64_t)by_time;

			decided = true;
			use_status = diff < 1024 && diff > -1024 && status_consumed != 0;
			snd_log("snd: after %llu us the DMA read %llu frames, the clock says %llu: following %s\n", now - t0,
			    status_consumed, by_time, use_status ? "the DMA" : "the clock");
		}
		consumed = base + (use_status ? status_consumed : by_time);
		if (written < consumed) {
			underruns++;
			written = consumed + 64;
		}
		while (written < consumed + LEAD_FRAMES) {
			uint32_t n = (uint32_t)(consumed + LEAD_FRAMES - written), room = BUF_FRAMES - (uint32_t)(written % BUF_FRAMES);

			n = n < CHUNK ? n : CHUNK;
			n = n < room ? n : room;
			fill(written, n);
			written += n;
		}
		__asm__ volatile ("dsb sy" ::: "memory");

		/* the amplifiers: on with sound, off after three quiet seconds; iOS's volume; their faults */
		if (amps_ok && !faulted && now - last_vol >= 100000) {
			bool sound = last_sound_us != 0 && now - last_sound_us < QUIET_OFF_US;
			uint32_t b = 0, g = 0;
			bool m = false;

			last_vol = now;
			if (redwood_audio_volume(&b, &m, &g) && g != generation) {
				generation = g;
				below = b;
				muted = m;
			}
			if (sound && !muted && !amps_on) {
				amps_on = amps_up();
				if (!amps_on) {
					faulted = true;
					amps_down();
				}
				quiet_since = 0;
				snd_log("snd: amplifiers %s (iOS volume %u/8 dB under)\n", amps_on ? "on" : "FAILED", below);
			} else if (amps_on && (!sound || muted)) {
				if (quiet_since == 0) {
					quiet_since = now;
				}
				if (muted || now - quiet_since >= 100000) {
					amps_down();
					amps_on = false;
					redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
					snd_log("snd: amplifiers off (%llu frames from iOS, %llu fed, %u underruns)\n", ios_frames, fed_frames,
					    (uint32_t)underruns);
				}
			} else if (sound) {
				quiet_since = 0;
			}
			/* (quiet_since is not touched here: clearing it on every pass kept the amplifiers on for good, 27 October) */
			if (amps_on) {
				uint32_t total = snd_below + (tone_left != 0 ? 0 : below);

				amps_volume(total, false);
				redwood_guard_attenuation(total);
				if (now - checked_at >= 500000) {
					uint32_t f = amps_faults();

					checked_at = now;
					if (f != 0) {
						snd_log("snd: AMPLIFIER FAULT %#x: both off for the rest of the run\n", f);
						amps_down();
						amps_on = false;
						faulted = true;
						redwood_guard_attenuation(REDWOOD_GUARD_SILENT);
					}
				}
			}
		}
		if (now - last_look >= 60000000) {
			last_look = now;
			snd_log("snd: %llu frames from iOS, %llu fed, %llu silent, %u underruns; DMA at %u (%s); quiet %llu times, "
			    "%llu ms in all\n", ios_frames, fed_frames, silent_frames, (uint32_t)underruns, pos,
			    use_status ? "followed" : "by the clock", quiet_spells, quiet_us / 1000);
		}
		if (decided && !amps_on && tone_left == 0 &&
		    (last_sound_us == 0 || now - last_sound_us >= QUIET_OFF_US)) {
			uint64_t began = now_us(), heard = last_sound_us;

			/* the whole ring silence, then asleep until iOS sends sound (it goes on sending silence: dropped) */
			bzero(buf, BUF_BYTES);
			__asm__ volatile ("dsb sy" ::: "memory");
			snd_quiet = true;
			quiet_spells++;
			while (last_sound_us == heard && now_us() - began < 60000000) {
				ring_tail = ring_head;
				assert_wait_timeout((event_t)&ring_head, THREAD_UNINT, 1000, NSEC_PER_MSEC);
				if (last_sound_us != heard) {
					clear_wait(current_thread(), THREAD_AWAKENED);
					break;
				}
				thread_block(THREAD_CONTINUE_NULL);
			}
			snd_quiet = false;
			now = now_us();
			quiet_us += now - began;
			/* on from where the DMA is now: the count again such that count % BUF_FRAMES is its ring index */
			pos = dma_position(NULL);
			base = written + (pos + BUF_FRAMES - (uint32_t)(written % BUF_FRAMES)) % BUF_FRAMES;
			written = base;
			last_pos = pos;
			status_consumed = 0;
			t0 = now;
			continue;
		}
		IOSleep(4);
	}
}

/* ---- entry points --------------------------------------------------------------------------------------------- */

void
redwood_exynos_sound_init(void)
{
	uint32_t on = 0;
	uint64_t pool_phys;
	vm_offset_t pool_virt;
	size_t pool_size;
	thread_t thread;

	if (!PE_parse_boot_argn("snd", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("snd_below", &snd_below, sizeof(snd_below));
	PE_parse_boot_argn("snd_gain", &snd_gain, sizeof(snd_gain));
	PE_parse_boot_argn("snd_tone", &snd_tone, sizeof(snd_tone));
	if (snd_gain > REDWOOD_GUARD_MAX_GAIN) {
		snd_gain = REDWOOD_GUARD_MAX_GAIN;
	}
	if (snd_tone > 3000) {
		snd_tone = 3000;
	}
	/* the DMA's ring: the last 64 KB of the loader's pool below 4 GB (the Wi-Fi driver only uses its first 4 MB) */
	if (redwood_wifi_dma_pool(&pool_phys, &pool_virt, &pool_size) < 0 || pool_size < (8U << 20)) {
		snd_log("snd: no DMA pool from the loader: no sound\n");
		return;
	}
	buf_phys = pool_phys + pool_size - 0x10000;
	buf = (int16_t *)ml_io_map_unmappable((vm_offset_t)buf_phys, 0x10000, VM_WIMG_WCOMB);
	pmu = (volatile uint8_t *)ml_io_map(PMU_PHYS, 0x2000);
	cmu = (volatile uint8_t *)ml_io_map(CMU_AUD_PHYS, 0x4000);
	mmu = (volatile uint8_t *)ml_io_map(SYSMMU_PHYS, 0x9000);
	abox = (volatile uint8_t *)ml_io_map(ABOX_PHYS, 0x2000);
	if (buf == NULL || pmu == NULL || cmu == NULL || mmu == NULL || abox == NULL) {
		snd_log("snd: cannot map\n");
		return;
	}
	snd_log("snd=1: the speakers through ABOX; DMA ring at %#llx\n", buf_phys);
	started = true;
	if (kernel_thread_start_priority(sound_thread, NULL, 90, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		started = false;
	}
}

/* at the run's end and on a panic's way out: the amplifiers into reset at once, the DMA stopped (its memory is reused) */
void
redwood_exynos_sound_quiesce(void)
{
	if (!started) {
		return;
	}
	if (gpb2 != NULL) {
		for (unsigned i = 0; i < 2; i++) {
			redwood_gpio_set(gpb2, amps[i].reset_pin, 0);
		}
	}
	if (abox != NULL && (rd(pmu, PMU_AUD_STATUS) & 1)) {
		upd(abox, RDMA(RDMA_CTRL0), 1, 0);
		upd(abox, ABOX_UAIF_CTRL0, 1, 0);
	}
	started = false;
}
