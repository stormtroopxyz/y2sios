/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). A "swap" copies the pixels of the surfaces of its layers into that memory, and
 * "vsync" is a timer. On the y2s (Exynos 990) two things of the display hardware are driven:
 * DECON's TE trigger, held while a frame is copied in (fb_te), and the panel's brightness, sent
 * as DCS commands through DSIM (fb_bl).
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);
extern "C" void loader_log_line(const char * line);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * The Exynos 990's display controller (y2s; Samsung's dpu30/cal_9830). The panel is a MIPI command-mode
 * one: on every TE pulse DECON_F reads the whole frame out of memory through one DMA channel, so pixels
 * written into the memory being read tear. Boot-arg fb_flip=1: two panel buffers, the frame drawn into
 * the one not being read, then that channel's base address switched to it (latched by DECON at the start
 * of the next frame) -- no frame is ever read half drawn. fb_dpu=1: only read the controller's state and
 * say it. Both are for this SoC only: the addresses are its own.
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
static TUNABLE(uint32_t, gDPU, "fb_dpu", 0);
#define DPU_DECON_F_PHYS        0x19050000ULL
/*
 * DPU_DMA: the common block at + 0, the IDMA of channel c at + 0x1000 * (c + 1) (dpp_drv.c: a dpp node's
 * first reg is its DMA; 0x19041000.. is the DPP colour pipe). Both read 0 to the non-secure side until
 * the DPU's D-TZPC is restored: fb_dpu=2 asks the secure monitor for that, as Linux does when it powers
 * pd-dpu on (pmucal_local.c, need_smc 0x19010204) and as redwood_mali.c does for the GPU's domain.
 */
#define DPU_DMA_PHYS            0x19090000ULL
#define DPU_DPP_PHYS            0x19040000ULL           /* DPP of channel c at + 0x1000 * (c + 1) */
#define DPU_DMA_VERSION         0x0000
#define DPU_DMA_QCH_EN          0x000C
#define DPU_DMA_GLB_CGEN_CH0    0x0040
#define SMC_PREPARE_PD_ONOFF    0x82000410UL
#define PD_DPU_NEED_SMC         0x19010204UL
#define DECON_GLOBAL_CONTROL    0x0000
#define DECON_SHADOW_UPDATE     0x0060
#define  DECON_SHADOW_GLOBAL    (1u << 31)
#define DECON_TRIG_CONTROL      0x0070
#define DECON_TE_COUNT          0x0078
#define DECON_DATA_PATH_0       0x0214                  /* window w enabled: bit 4w */
#define DECON_DATA_PATH_1       0x0218                  /* window w's channel: bits 4w..4w+2 */
#define IDMA_ENABLE_REG         0x0000
#define IDMA_IN_CON_REG         0x0008
#define IDMA_SRC_SIZE_REG       0x0010
#define IDMA_SRC_OFFSET_REG     0x0014
#define IDMA_IMG_SIZE_REG       0x0018
#define IDMA_BASE_Y8_REG        0x0040
#define IDMA_STRIDE_REG         0x0050
#define IDMA_SHADOW             0x0800                  /* what the DMA is using now, read back */
#define FLIP_LATCH_MS           50

/*
 * fb_scaler=1 (PLANS/07-display.md, D4): no copy at all. CoreAnimation's frame (1125x2436 BGRA, one
 * piece of physical memory: MiniIOSurface.cpp, fb_scaler) is read by the display controller itself
 * through channel 5 (VGRFS, one of the two with a scaler) and scaled there to the panel's 1440x3200.
 * The window S-Boot set up stays as it is -- full screen, opaque -- and only the channel it reads
 * changes: DATA_PATH_CONTROL_1's field for it, from S-Boot's channel to 5, latched with the next
 * frame by a shadow update like fb_flip's base. A frame that is anything else (more layers, another
 * size, memory not in one piece) is drawn by the processor as before, and the window goes back to
 * S-Boot's channel. Channel 5 is set up like S-Boot's (its format, bus and QoS settings copied) plus
 * the scaler: sizes, ratios, and a bicubic filter of our own. Its registers are Samsung's cal_9830
 * (regs-dpp.h, dpp_reg.c): IDMA5 at 0x19096000, DPP5 at 0x19046000.
 */
static TUNABLE(uint32_t, gScaler, "fb_scaler", 0);
/*
 * fb_swapwait=1: with the scaler on, SwapEnd returns only once its frame is on the panel (run 17's cure for
 * tearing: CoreAnimation draws its next frame into the surface the panel showed).
 * By default SwapEnd returns once the swap is queued and the wait is where CoreAnimation asks for it: before
 * each swap it calls IOMobileFramebufferSwapWait for the swaps before (`tl: w N-2`, `w N-1`, then `s N`:
 * run 27), and only then draws into the surface that swap N-2 had. kFBMethodSwapWait answers once the swap
 * asked about is on the panel (fShownSwapID), so the drawing of a frame overlaps the panel's wait for the one
 * before, as with Apple's driver. present() counts a swap whose surface is still shown or queued (would tear).
 * Measured (runs 27-28, PLANS/10-mali2metal.md): no gain -- 45 % of animation frames at 60 Hz against 48 %
 * with the swap waiting -- because what spaces the frames is the window server's own time, so the swap
 * waits again by default (fb_swapwait=1) and fb_swapwait=0 keeps the protocol for when that changes.
 */
static TUNABLE(uint32_t, gSwapWait, "fb_swapwait", 1);
#define SCALER_CH               5
#define IDMA_IRQ_REG            0x0004
#define  IDMA_IRQ_ERRORS        ((1u << 26) | (1u << 25) | (1u << 24) | (1u << 23) | (1u << 22) | (1u << 21) | (1u << 19) | (1u << 17))
#define  IDMA_IRQ_PENDING       (0x07FBu << 16)
#define  IDMA_SFR_UPDATE_FORCE  (1u << 4)
#define IDMA_STRIDE_1_REG       0x0054
#define IDMA_IN_REQ_DEST_REG    0x0308
#define DPP_ENABLE_REG          0x0000
#define  DPP_SFR_UPDATE_FORCE   (1u << 4)
#define  DPP_ENABLE_COPIED      ((7u << 8) | (1u << 5) | (1u << 3))     /* clock gates, PSLVERR, Q-channel */
#define DPP_IRQ_REG             0x0004
#define  DPP_IRQ_CONFIG_ERROR   (1u << 21)
#define  DPP_IRQ_PENDING        (0x21u << 16)
#define DPP_IN_CON_REG          0x0008
#define DPP_IMG_SIZE_REG        0x0018
#define DPP_SCALED_IMG_SIZE_REG 0x002C
#define DPP_MAIN_H_RATIO_REG    0x0044
#define DPP_MAIN_V_RATIO_REG    0x0048
#define DPP_V_COEF(n, s, x)     (0x200 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_H_COEF(n, s, x)     (0x290 + (n) * 0x4 + (s) * 0x24 + (x) * 0x200)
#define DPP_POSITION_REG(i)     (0x05B0 + 4 * (i))                     /* Y h/v, C h/v: the first phase */
#define DPP_DYNAMIC_GATING_REG  0x0A54
#define DPP_CFG_ERR_STATE_REG   0x0D08
#define DECON_SHADOW_REGS       0x7000                  /* DECON's registers in use, read back */
/*
 * Keys' bicubic (a = -0.5) at phases 0..8 of 16 (the rest are these mirrored), each summing to 512,
 * the scale Samsung's tables use (dpp_coef.h); 11-bit two's complement in the registers. Four taps,
 * the centre one second (as v_coef_4t); horizontally the same four in the middle of eight, the
 * centre fourth (as h_coef_8t).
 */
static const int16_t scalerBicubic[9][4] = {
	{    0,  512,    0,    0 },
	{  -14,  507,   20,   -1 },
	{  -24,  494,   46,   -4 },
	{  -32,  472,   79,   -7 },
	{  -36,  444,  116,  -12 },
	{  -38,  410,  157,  -17 },
	{  -38,  372,  200,  -22 },
	{  -35,  331,  244,  -28 },
	{  -32,  288,  288,  -32 },
};

/*
 * fb_te=1: no tearing without the DMA's registers (they read 0 here: gated). Frames are drawn into
 * a back buffer of ours; then DECON's hardware trigger is masked (the panel keeps showing its own
 * memory), DECON is waited for until it is not sending a frame, the changed rows are copied into the
 * buffer it reads, and the trigger is let go: the next TE sends the whole new frame.
 */
static TUNABLE(uint32_t, gTE, "fb_te", 0);

/*
 * Boot-arg fb_irq (default 1): the waits for DECON to take a new frame (its shadow registers latch at the
 * start of a frame) sleep until its frame-start interrupt (Samsung's dt: decon_f interrupts, the first: SPI
 * 0xfe, level; decon_irq_handler clears INTERRUPT_PENDING bit 12) instead of looking every 100 us -- each
 * look a timer, some 2800 a second on the big cores (the cpuidle wake record, 27 October). Unmasked only
 * while somebody waits: the panel's frames start on every TE whether anything changed or not.
 * fb_irq=0: the polling, as before.
 */
static TUNABLE(uint32_t, gFrameIrq, "fb_irq", 1);
#define DECON_INTID_FRAME_START (32 + 0xfe)
#define DECON_INT_EN_ALL        (1u << 0)               /* INTERRUPT_ENABLE: DPU_INT_EN */
#define DECON_INT_FRAME_DONE    (1u << 13)
#define DECON_INT_EXTRA         (1u << 4)
/* a wait sleeps at most this long between looks: a 60 Hz refresh (27 October: no frame-start interrupt was
 * missed, 0 waits gave up; at 4 ms, 35 of 59 waits looked again for nothing) */
#define LATCH_CAP_MS            17
extern "C" kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid),
    void *context);
extern "C" void gic_v3_spi_mask(uint32_t intid);
extern "C" void gic_v3_spi_unmask(uint32_t intid);
#define DECON_GLOBAL_IDLE       (1u << 5)               /* no frame being sent */
#define DECON_GLOBAL_RUN        (1u << 4)
#define DECON_GLOBAL_CMD_MODE   (1u << 8)
#define DECON_INT_ENABLE        0x0040
#define DECON_INT_PENDING       0x004C
#define DECON_SECURE_CONTROL    0x0064
#define DECON_TRIG_TIMER        0x0074
#define  DECON_TRIG_HW_EN       (1u << 0)
#define  DECON_TRIG_MASK        (1u << 4)               /* HW_TRIG_MASK_DECON */
#define DECON_TRIG_SECURE       0x007C
#define DECON_FRAME_COUNT       0x02A0
#define DECON_OUTFIFO_COUNTER0  0x0474                  /* bits 16..29: the line being sent */
#define DECON_INT_FRAME_START   (1u << 12)              /* INTERRUPT_PENDING: a frame has started (write 1: cleared) */
#define TE_IDLE_WAIT_MS         40
#define TE_SETTLE_US            30

/*
 * fb_bl=1: the brightness CoreBrightness asks for ("IOMFBBrightnessLevel", nits in 16.16) sent to
 * the panel as DCS commands through DSIM0's command FIFO. The panel is Samsung's s6e3hab "hubble3":
 * brightness is its register 6A (DBV, 11 bits), set between the level-2 key F0 5A 5A / F0 A5 A5 and
 * latched by F7 0F (Samsung's exynos990 kernel, panel/s6e3hab/s6e3hab_hubble3_a3_s0_panel*.h).
 */
static TUNABLE(uint32_t, gBL, "fb_bl", 0);
#define DSIM0_PHYS              0x190C0000ULL
#define DSIM_LINK_STATUS1       0x000C
#define DSIM_CONFIG             0x004C
#define DSIM_INTSRC             0x0050
#define DSIM_PKTHDR             0x0058
#define DSIM_PAYLOAD            0x005C
#define DSIM_FIFOCTRL           0x0068
#define  DSIM_FIFO_PL_FULL      (1u << 9)
#define  DSIM_FIFO_PH_EMPTY     (1u << 10)
#define  DSIM_FIFO_PH_FULL      (1u << 11)
#define DSIM_CMD_CONFIG         0x0080
#define DSIM_RXFIFO             0x0060
#define  DSIM_FIFO_RX_EMPTY     (1u << 12)
#define  DSIM_INT_RX_DONE       (1u << 18)
#define  DSIM_PKTHDR_BTA        (1u << 24)
#define DSI_DCS_SHORT_WRITE     0x05
#define DSI_DCS_SHORT_WRITE_1   0x15
#define DSI_DCS_LONG_WRITE      0x39
#define DSI_DCS_READ            0x06
#define DSI_MAX_RETURN_SIZE     0x37
#define DSI_RX_ERROR_REPORT     0x02
#define DSI_RX_SHORT_1          0x21
#define DSI_RX_SHORT_2          0x22
#define DSI_RX_LONG             0x1C
#define  DSIM_CMD_PKT_GO_EN     (1u << 16)
/*
 * hubble2's brightness is its gamma (CA) and AOR (B1: of every frame, the lines the OLEDs are held
 * off), Samsung's tables of both come from the panel's own flash (s6e3hab.c init_aor_table, DIM_FLASH),
 * and 6A (DBV) alone does nothing. Without the gamma: the AOR as a dimmer, from what the panel was
 * found with (its brightness then) to Samsung's darkest (0x0C77 of some 3200 lines held off at 2 nits).
 */
#define PANEL_AOR_DARKEST       0x0C27                  /* (Samsung's 0x0C77 leaves 9 of 3200 lines lit: the colours go red) */
#define PANEL_AOR_BRIGHTEST     0x01E4                  /* Samsung's AOR from 265 nits up (AOR 0 makes the panel white) */
#define kBacklightMax           65536
#define kPanelMaxNits           420

/* hubble3: nits of each brightness step, and the DBV that gives it */
static const uint16_t gPanelNits[] = {
	2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 24, 25, 27, 29, 30, 32, 34, 37, 39, 41,
	44, 47, 50, 53, 56, 60, 64, 68, 72, 77, 82, 87, 93, 98, 105, 111, 119, 126, 134, 143, 152, 162, 172, 183, 195, 207,
	220, 234, 249, 265, 282, 300, 316, 333, 350, 357, 365, 372, 380, 387, 395, 403, 412, 420,
};
static const uint16_t gPanelDBV[] = {
	0x00A, 0x00F, 0x014, 0x018, 0x01D, 0x022, 0x027, 0x02B, 0x030, 0x035, 0x03A, 0x03E, 0x043, 0x048, 0x04D, 0x052, 0x05B,
	0x060, 0x065, 0x06A, 0x073, 0x078, 0x082, 0x08C, 0x090, 0x09A, 0x0A4, 0x0B2, 0x0BC, 0x0C6, 0x0D4, 0x0E3, 0x0F1, 0x100,
	0x10E, 0x121, 0x135, 0x148, 0x15C, 0x176, 0x18F, 0x1A8, 0x1C7, 0x1E0, 0x202, 0x21E, 0x245, 0x266, 0x28D, 0x2B8, 0x2E3,
	0x313, 0x343, 0x378, 0x3B3, 0x3ED, 0x42C, 0x471, 0x4BA, 0x508, 0x55A, 0x5B1, 0x5FE, 0x650, 0x6A4, 0x6C7, 0x6EF, 0x711,
	0x739, 0x75C, 0x783, 0x7AB, 0x7D7, 0x7FF,
};
static_assert(sizeof(gPanelNits) / sizeof(gPanelNits[0]) == sizeof(gPanelDBV) / sizeof(gPanelDBV[0]), "one DBV per step");

/* A line for the loader's log (IOLog's buffer reaches it only when the kernel returns in order) and for IOLog. */
static void fbSay(const char * format, ...) __printflike(1, 2);
static void
fbSay(const char * format, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	IOLog("%s", line);
	loader_log_line(line);
}

static inline uint32_t
dpuRead(volatile uint8_t * base, uint32_t offset)
{
	return *(volatile uint32_t *) (base + offset);
}

static inline void
dpuWrite(volatile uint8_t * base, uint32_t offset, uint32_t value)
{
	*(volatile uint32_t *) (base + offset) = value;
}

static uint64_t
dpuSmc(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* a frame the GPU has not drawn by then is shown as it is */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
/*
 * Boot-arg fb_damage=0: every swap scales the whole frame onto the panel, as before. By default,
 * when the bottom layer is the only one, a copy of the picture last shown is kept and only the
 * panel rows over rows of the picture that changed are drawn again (a clock that ticks: a few
 * dozen rows of 3200). Comparing reads cached memory only; the panel's is slow to read and is never read.
 */
static TUNABLE(uint32_t, gDamage, "fb_damage", 1);
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	uint32_t        fSwapsQueued, fSwapsDone;       /* tickets: present() hands them out, the presenter retires them */
	uint64_t        fLatchTime;                     /* when the scaler's last frame was seen taken: the panel's phase */
	bool            fSwapQueuedUse[SWAP_QUEUE];     /* the slot's surface was counted in use when queued */
	uint32_t        fStatSwapWaitTimeouts;
	uint32_t        fShownSwapID, fQueuedSwapID;    /* the last swap on the panel, and the last queued (SwapWait) */
	uint32_t        fShownSid, fNextSid;            /* the surfaces the panel reads and has been asked for (ids) */
	uint32_t        fStatIntoShown;                 /* swaps whose surface was still shown or queued: would tear */
	/* what the swaps were (fb_swap line): which bottom-layer surfaces, how many asked for what the one before did */
	uint8_t         fPrevSwapLayers[SWAP_LAYERS_CLEARED + 4 - SWAP_SURFACE(0)];
	uint32_t        fStatSwapSame;                  /* swaps with the same surfaces, rectangles and layers as the one before */
	uint32_t        fStatSwapSids[8], fStatSwapSidsMore;    /* the bottom layer's different surfaces; beyond 8 */
	int             fStatSwapPid;                   /* who swapped last */
	uint32_t        fStatSwapWaits, fStatSwapWaitsBlocked, fStatSwapWaitTimeouts2;
	uint64_t        fStatSwapWaitTime;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	/* fb_damage: the picture of the bottom layer as last put on the panel, and where it went */
	uint8_t *       fShadow;
	size_t          fShadowSize;
	bool            fShadowValid;
	int32_t         fShadowKey[10];         /* sx, sy, sw, sh, px0, py0, px1, py1, filter, panel width */
	uint64_t        fStatRows, fStatRowsFull;       /* panel rows drawn, and rows a full frame each would have been */
	uint32_t        fStatSkipped;           /* frames in which nothing had changed */
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */
	/* fb_flip: the display controller, the two panel buffers, and which rows each still lacks */
	volatile uint8_t * fDecon;
	volatile uint8_t * fDma;                /* the IDMA of the channel that reads the panel's window */
	int             fWin;                   /* that window, -1: none found */
	bool            fFlipWanted, fFlip, fFlipPending, fFrameDrew, fShadowReadable;
	uint8_t *       fBuf[2];
	uint64_t        fBufPhys[2];
	unsigned        fFront;                 /* the buffer being read */
	int32_t         fPrevRy0, fPrevRy1;     /* rows the last frame changed: the buffer drawn next lacks them */
	int32_t         fOwnRy0, fOwnRy1;       /* rows this frame changes */
	uint64_t        fFlipStart;
	uint32_t        fStatFlips, fStatFlipLate;
	uint64_t        fStatFlipWait;
	char            fDpuWhy[160];           /* fb_flip: why it is not on, said again with every report */
	/* fb_scaler */
	volatile uint8_t * fDma5;               /* channel 5's IDMA and DPP */
	volatile uint8_t * fDpp5;
	volatile uint8_t * fDppT;               /* the DPP of S-Boot's channel: what channel 5 copies */
	unsigned        fScanCh;                /* S-Boot's channel, which the window goes back to */
	bool            fScalerReady, fScalerOn, fScanPending;
	MiniIOSurface * fScanShown;             /* the surface the panel reads (held, and in use for its maker) */
	MiniIOSurface * fScanNext;              /* the one asked for, taken at the next frame */
	uint32_t        fScanAsked;             /* its address */
	uint64_t        fScanStart;
	uint32_t        fStatScan, fStatScanCpu, fStatScanNotTaken;
	uint64_t        fStatScanWait;
	char            fScalerWhy[200];
	void scalerSetup(void);
	bool scalerFits(const uint8_t * swap, MiniIOSurface ** surfaces);
	bool scalerWait(void);
	void scalerShow(MiniIOSurface * s);
	void loaderQuiesce(void);
	void scalerOff(const char * why, bool forever);
	char            fDpuDump[512];          /* fb_dpu/fb_flip: what the controller was found doing, likewise */
	/* fb_te: frames drawn into fBack, then copied under a masked trigger into fPanel (the buffer DECON reads) */
	bool            fTe;
	uint8_t *       fPanel;                 /* the boot framebuffer itself */
	uint8_t *       fBack;
	size_t          fBackSize;
	uint32_t        fTeTrig;                /* HW_SW_TRIG_CONTROL as found (the trigger not masked) */
	uint32_t        fTeFrames, fTeRaced, fTeTimeouts, fTeNeverIdle, fTeLate;
	bool            fTeStartFlag;           /* INTERRUPT_PENDING's frame-start bit can be cleared and comes back with a frame */
	uint64_t        fTeIdleWait, fTeIdleWaitMax, fTeCopy, fTeRows;
	uint32_t        fTeCount0, fTeFrameCount0;      /* TE and frame counters at the last report */
	uint64_t        fTeSince;
	/* fb_bl: the panel's brightness through DSIM0 */
	volatile uint8_t * fDsim;
	IOLock *        fBLLock;
	thread_call_t   fBLCall;
	uint32_t        fBLTarget;              /* nits in 16.16, as CoreBrightness gives it */
	uint32_t        fBLSent;                /* DBV last sent, 0: none */
	uint32_t        fBLRequests, fBLWrites, fBLFailures;
	bool            fBLOff;

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void swapWait(uint32_t id);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool damage);
	bool damageRows(const struct RowJob * job, int32_t * y0, int32_t * y1);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
	void dpuProbe(void);
	void flipEnable(void);
	void flipWait(void);
	void flipNow(void);
	void runJob(struct RowJob * job);
	void teSetup(void);
	bool            fFrameIrq;              /* fb_irq: the frame-start interrupt is attached */
	volatile uint32_t fFrameStarts;         /* frame starts it has seen */
	uint32_t        fStatLatchIrq, fStatLatchCap, fStatLatchLimit;
	void frameIrqSetup(void);
	static void frameIrq(void * context, uint32_t intid);
	bool latchWait(uint32_t mask, uint64_t start);
	uint32_t frameStartMark(void);
	bool frameStartSince(uint32_t mark);
	void teMeasure(void);
	void tePresent(int32_t y0, int32_t y1);
	void teReport(uint64_t nanos);
	void blSetup(void);
	bool dsimWrite(const uint8_t * bytes, uint32_t length);
	int dsimRead(uint8_t command, uint8_t * out, uint32_t max);
	void panelReport(const char * when);
	bool blSend(uint32_t dbv, uint32_t aor);
	uint32_t aorRead(void);
	uint32_t        fAor0;                  /* the AOR (B1) the panel was found with: its full brightness */
	bool            fPacketGo;              /* DSIM writes with PKT_GO, as Samsung's dsim_write_data() does */
	void brightnessRequest(uint32_t level);
	void displayPower(bool on);
	IOLock *        fDsimLock;              /* one DSIM command sequence at a time */
	bool            fDsimReads;             /* DCS reads work (a read timed out: none tried again) */
	bool            fDisplayOff;
	static void blWork(thread_call_param_t self, thread_call_param_t unused);
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
	/* where a frame's time in the driver goes (5 s sums) */
	uint64_t        fStatCompare, fStatScale;
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);
static IOMobileFramebuffer * gFramebuffer;      /* the one there is (redwood_fb_quiesce) */
static volatile uint32_t gBLThermalCap;
/* the window server's refresh notifications (kFBNotifyVSync): asked for, sent, not sent (no port, or closed) */
static volatile UInt32 gVSyncAsked, gVSyncSent, gVSyncStopped;         /* nits in 16.16 the panel is held under while the chip is hot; 0: none */

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	gFramebuffer = this;
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth);
	fColumnWeight = IONewZero(uint16_t, fPanelWidth);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);
	fWin = -1;
	fPanel = fBase;
	if (gFlip || gDPU || gScaler) {
		dpuProbe();
	}
	if (gTE && !gFlip) {
		teSetup();
	}
	if (gFrameIrq) {
		frameIrqSetup();        /* (after teSetup: its measuring reads the frame-start flag itself) */
	}
	blSetup();

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1;
	int32_t         ry0, ry1;               /* the panel rows to draw: all of py0..py1, or those fb_damage found changed */
	bool            filter, unscaled;
	bool            copy;                   /* fb_te: not drawing but rows ry0..ry1 of the back buffer to the panel */
	bool            compare;                /* fb_damage: not drawing but picture rows ry0..ry1 compared with fShadow */
	int32_t *       changed;                /* ... the first and last that differed, gathered from every share */
};

static bool rowsDiffer(const uint8_t * a, const uint8_t * b, size_t bytes);

static void
atomicMin(int32_t * at, int32_t value)
{
	int32_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);

	while (value < seen && !__atomic_compare_exchange_n(at, &seen, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

static void
atomicMax(int32_t * at, int32_t value)
{
	int32_t seen = __atomic_load_n(at, __ATOMIC_RELAXED);

	while (value > seen && !__atomic_compare_exchange_n(at, &seen, value, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
	}
}

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;

	if (job->compare) {
		size_t rowBytes = (size_t) sw * 4;
		int32_t first = -1, last = -1;

		for (int32_t r = y0; r < y1; r++) {
			const uint8_t * now = pixels + (size_t) (sy + r) * stride + (size_t) sx * 4;
			uint8_t * then = fShadow + (size_t) r * rowBytes;

			if (rowsDiffer(now, then, rowBytes)) {
				memcpy(then, now, rowBytes);
				first = first < 0 ? r : first;
				last = r;
			}
		}
		if (first >= 0) {
			atomicMin(&job->changed[0], first);
			atomicMax(&job->changed[1], last);
		}
		return;
	}
	if (job->copy) {
		for (int32_t y = y0 < 0 ? 0 : y0; y < y1 && (uint32_t) y < fPanelHeight; y++) {
			memcpy(fPanel + (size_t) y * fRowBytes, fBack + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
		}
		return;
	}
	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				for (uint32_t c = first; c <= last; c++) {
					uint32_t a = upper[c], b = lower[c];

					mixed[c] = a == b ? a : blend(a, b, weight);           /* (most of a screen is the same as next to it) */
				}
				from = blendRow;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = to + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				to[x] = from[fColumn[x]];
			}
		}
	}
}

/*
 * fb_dpu / fb_flip: what the bootloader left the display controller doing. The window that is on
 * (DATA_PATH_CONTROL_0), the DMA channel it reads (DATA_PATH_CONTROL_1) and where that channel reads
 * from; fb_flip goes on only if that is the boot framebuffer itself, i.e. the DMA sees physical
 * addresses (no SYSMMU in the way) and the window is the one the panel shows.
 */
void
IOMobileFramebuffer::dpuProbe(void)
{
	uint32_t paths0, paths1, ch, base, shadow;
	int on = 0;

	fDecon = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DECON_F_PHYS, 0x10000);
	if (fDecon == NULL) {
		IOLog("MiniIOMobileFramebuffer: dpu: DECON_F not mapped\n");
		strlcpy(fDpuWhy, "DECON_F not mapped", sizeof(fDpuWhy));
		return;
	}
	paths0 = dpuRead(fDecon, DECON_DATA_PATH_0);
	paths1 = dpuRead(fDecon, DECON_DATA_PATH_1);
	{
		volatile uint8_t * com = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DMA_PHYS, 0x1000);
		volatile uint8_t * dma0 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000), 0x1000);
		volatile uint8_t * dpp0 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000), 0x1000);

		for (int round = 0; com != NULL && dma0 != NULL && dpp0 != NULL && round < 2; round++) {
			fbSay("fb: dpu: %s: DMA common version %#x qch %#x cgen %#x; IDMA0 enable %#x size %#x base %#x (in use %#x); "
			    "DPP0 enable %#x; DECON global %#x trig %#x\n", round ? "after the D-TZPC restore" : "as found",
			    dpuRead(com, DPU_DMA_VERSION), dpuRead(com, DPU_DMA_QCH_EN), dpuRead(com, DPU_DMA_GLB_CGEN_CH0),
			    dpuRead(dma0, IDMA_ENABLE_REG), dpuRead(dma0, IDMA_SRC_SIZE_REG), dpuRead(dma0, IDMA_BASE_Y8_REG),
			    dpuRead(dma0, IDMA_BASE_Y8_REG + IDMA_SHADOW), dpuRead(dpp0, 0), dpuRead(fDecon, DECON_GLOBAL_CONTROL),
			    dpuRead(fDecon, DECON_TRIG_CONTROL));
			if (round == 0) {
				if (gDPU < 2) {
					break;
				}
				fbSay("fb: dpu: smc(%#lx, 1, %#lx, 2) = %#llx\n", SMC_PREPARE_PD_ONOFF, PD_DPU_NEED_SMC,
				    dpuSmc(SMC_PREPARE_PD_ONOFF, 1, PD_DPU_NEED_SMC, 2));
			}
		}
	}
	{
		/* every input DMA channel's enable, size and base (and the base in use), and DECON's own state */
		size_t at = (size_t) snprintf(fDpuDump, sizeof(fDpuDump), "DECON global %#x trig %#x shadow %#x te %#x paths %#x/%#x;",
		    dpuRead(fDecon, DECON_GLOBAL_CONTROL), dpuRead(fDecon, DECON_TRIG_CONTROL), dpuRead(fDecon, DECON_SHADOW_UPDATE),
		    dpuRead(fDecon, DECON_TE_COUNT), paths0, paths1);
		for (unsigned c = 0; c < 6 && at < sizeof(fDpuDump); c++) {
			volatile uint8_t * dma = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000ULL * (c + 1)), 0x1000);

			if (dma == NULL) {
				continue;
			}
			at += (size_t) snprintf(fDpuDump + at, sizeof(fDpuDump) - at, " ch%u en %#x size %#x base %#x/%#x;", c,
			    dpuRead(dma, IDMA_ENABLE_REG), dpuRead(dma, IDMA_SRC_SIZE_REG), dpuRead(dma, IDMA_BASE_Y8_REG),
			    dpuRead(dma, IDMA_BASE_Y8_REG + IDMA_SHADOW));
		}
		loader_log_line("MiniIOMobileFramebuffer: dpu: ");
		loader_log_line(fDpuDump);
		loader_log_line("\n");
	}
	IOLog("MiniIOMobileFramebuffer: dpu: DECON_F global %#x trigger %#x shadow %#x TE count %#x, windows on %#x, channels %#x\n",
	    dpuRead(fDecon, DECON_GLOBAL_CONTROL), dpuRead(fDecon, DECON_TRIG_CONTROL), dpuRead(fDecon, DECON_SHADOW_UPDATE),
	    dpuRead(fDecon, DECON_TE_COUNT), paths0, paths1);
	for (int w = 0; w < 6; w++) {
		if (paths0 & (1u << (4 * w))) {
			fWin = w;
			on++;
		}
	}
	if (on != 1) {
		IOLog("MiniIOMobileFramebuffer: dpu: %d windows on, not one: left alone\n", on);
		snprintf(fDpuWhy, sizeof(fDpuWhy), "%d windows on (paths %#x %#x)", on, paths0, paths1);
		fWin = -1;
		return;
	}
	ch = (paths1 >> (4 * fWin)) & 7;
	if (ch > 5) {
		IOLog("MiniIOMobileFramebuffer: dpu: window %d reads channel %u, which has no IDMA: left alone\n", fWin, ch);
		snprintf(fDpuWhy, sizeof(fDpuWhy), "window %d on channel %u", fWin, ch);
		fWin = -1;
		return;
	}
	fDma = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000ULL * (ch + 1)), 0x1000);
	if (fDma == NULL) {
		fWin = -1;
		return;
	}
	if (gScaler && !gFlip) {
		fScanCh = ch;
		scalerSetup();
	}
	base = dpuRead(fDma, IDMA_BASE_Y8_REG);
	shadow = dpuRead(fDma, IDMA_BASE_Y8_REG + IDMA_SHADOW);
	IOLog("MiniIOMobileFramebuffer: dpu: window %d, channel %u: IDMA enable %#x in %#x size %#x offset %#x image %#x "
	    "stride %#x, base %#x (in use %#x); the boot framebuffer is at %#llx\n", fWin, ch, dpuRead(fDma, IDMA_ENABLE_REG),
	    dpuRead(fDma, IDMA_IN_CON_REG), dpuRead(fDma, IDMA_SRC_SIZE_REG), dpuRead(fDma, IDMA_SRC_OFFSET_REG),
	    dpuRead(fDma, IDMA_IMG_SIZE_REG), dpuRead(fDma, IDMA_STRIDE_REG), base, shadow, vinfo.v_physaddr);
	if (!gFlip) {
		return;
	}
	if ((uint64_t) shadow != vinfo.v_physaddr && (uint64_t) base != vinfo.v_physaddr) {
		IOLog("MiniIOMobileFramebuffer: dpu: the channel does not read the boot framebuffer (an SMMU address?): no fb_flip\n");
		snprintf(fDpuWhy, sizeof(fDpuWhy), "window %d channel %u reads %#x / %#x, not the boot framebuffer %#llx", fWin, ch, base, shadow, vinfo.v_physaddr);
		return;
	}
	fShadowReadable = (uint64_t) shadow == vinfo.v_physaddr;
	/* the second buffer: one piece, below 4 GB (the register holds 32 bits), written through the write buffer */
	IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMapWriteCombineCache,
	    (mach_vm_size_t) fRowBytes * fPanelHeight, 0x00000000FFFFF000ULL);
	if (memory == NULL) {
		IOLog("MiniIOMobileFramebuffer: dpu: no memory for a second panel buffer: no fb_flip\n");
		strlcpy(fDpuWhy, "no memory for a second buffer", sizeof(fDpuWhy));
		return;
	}
	fBuf[0] = (uint8_t *) vinfo.v_baseaddr;
	fBufPhys[0] = vinfo.v_physaddr;
	fBuf[1] = (uint8_t *) memory->getBytesNoCopy();
	fBufPhys[1] = memory->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	fFlipWanted = true;
	IOLog("MiniIOMobileFramebuffer: dpu: fb_flip: frames go to %#llx and %#llx in turn\n", fBufPhys[0], fBufPhys[1]);
}

/* The first picture: from here on the frame is drawn where the panel is not reading. */
void
IOMobileFramebuffer::flipEnable(void)
{
	for (uint32_t y = 0; y < fPanelHeight; y++) {
		memcpy(fBuf[1] + (size_t) y * fRowBytes, fBuf[0] + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
	}
	fFront = 0;
	fBase = fBuf[1];
	fPrevRy0 = fPrevRy1 = 0;
	fFlip = true;
}

/* Before drawing: the switch to the frame before has been taken, so the buffer drawn into is nobody's. */
void
IOMobileFramebuffer::flipWait(void)
{
	uint32_t mask = DECON_SHADOW_GLOBAL | (1u << fWin);
	uint64_t began = mach_absolute_time(), limit, waited;

	if (!fFlipPending) {
		return;
	}
	(void) limit;
	latchWait(mask, fFlipStart);
	waited = mach_absolute_time() - began;
	fStatFlipWait += waited;
	fFlipPending = false;
	uint32_t inUse = dpuRead(fDma, IDMA_BASE_Y8_REG + IDMA_SHADOW);
	bool taken = fShadowReadable ? inUse == (uint32_t) fBufPhys[fFront] :
	    (dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) == 0;
	if (fStatFlips <= 4 || !taken) {
		uint64_t since;

		absolutetime_to_nanoseconds(mach_absolute_time() - fFlipStart, &since);
		IOLog("MiniIOMobileFramebuffer: dpu: flip %u: the channel reads %#x (asked %#llx) %llu us after the switch; shadow %#x\n",
		    fStatFlips, inUse, fBufPhys[fFront], since / 1000, dpuRead(fDecon, DECON_SHADOW_UPDATE));
	}
	if (!taken && ++fStatFlipLate >= 3) {
		/* the controller does not take the switch: back to one buffer, the boot framebuffer */
		IOLog("MiniIOMobileFramebuffer: dpu: the switch is not taken (%u times): fb_flip off\n", fStatFlipLate);
		snprintf(fDpuWhy, sizeof(fDpuWhy), "the switch was not taken (in use %#x, shadow %#x)", inUse, dpuRead(fDecon, DECON_SHADOW_UPDATE));
		dpuWrite(fDma, IDMA_BASE_Y8_REG, (uint32_t) fBufPhys[0]);
		dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | mask);
		fFlip = fFlipWanted = false;
		fBase = fBuf[0];
		fShadowValid = false;
	}
}

/* After drawing: the panel reads the buffer just drawn from its next frame on. */
void
IOMobileFramebuffer::flipNow(void)
{
	unsigned drawn = fFront ^ 1;

	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDma, IDMA_BASE_Y8_REG, (uint32_t) fBufPhys[drawn]);
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | DECON_SHADOW_GLOBAL | (1u << fWin));
	fFlipStart = mach_absolute_time();
	fFlipPending = true;
	fFront = drawn;
	fBase = fBuf[drawn ^ 1];
	fPrevRy0 = fOwnRy0;
	fPrevRy1 = fOwnRy1;
	fStatFlips++;
}

/*
 * fb_te: how DECON sends frames, watched for 100 ms before the first one: the TE pulses, the frames
 * sent, how long each takes (the IDLE bit of GLOBAL_CONTROL is clear while one is being sent) and how
 * long after its TE it starts; then 60 ms with the trigger masked, in which no frame should go out.
 */
/*
 * fb_scaler: channel 5 made ready once -- S-Boot's channel's settings, this picture's sizes, the
 * scaler -- while it is in nobody's path. The window keeps reading S-Boot's channel until the first
 * frame that fits (scalerShow()).
 */
void
IOMobileFramebuffer::scalerSetup(void)
{
	static const uint32_t idmaCopied[] = {
		IDMA_IN_CON_REG, IDMA_STRIDE_REG, IDMA_STRIDE_1_REG, 0x0064 /* SBWC */, 0x0070 /* recovery */,
		0x0100 /* deadlock */, 0x0110 /* bus */, 0x0114 /* cache */, 0x0120 /* performance */, 0x0130, 0x0134 /* QoS */,
		0x0140 /* dynamic gating */,
	};
	uint32_t paths0 = dpuRead(fDecon, DECON_DATA_PATH_0), paths1 = dpuRead(fDecon, DECON_DATA_PATH_1);
	uint32_t w = fWidth, h = fHeight, pw = fPanelWidth, ph = fPanelHeight, a, b;

	if (fScanCh == SCALER_CH) {
		snprintf(fScalerWhy, sizeof(fScalerWhy), "S-Boot's window reads channel %u already", SCALER_CH);
		goto refused;
	}
	for (int win = 0; win < 6; win++) {
		if ((paths0 & (1u << (4 * win))) && ((paths1 >> (4 * win)) & 7) == SCALER_CH) {
			snprintf(fScalerWhy, sizeof(fScalerWhy), "window %d reads channel %u", win, SCALER_CH);
			goto refused;
		}
	}
	fDma5 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DMA_PHYS + 0x1000ULL * (SCALER_CH + 1)), 0x1000);
	fDpp5 = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000ULL * (SCALER_CH + 1)), 0x1000);
	fDppT = (volatile uint8_t *) ml_io_map((vm_offset_t) (DPU_DPP_PHYS + 0x1000ULL * (fScanCh + 1)), 0x1000);
	if (fDma5 == NULL || fDpp5 == NULL || fDppT == NULL) {
		strlcpy(fScalerWhy, "channel 5 not mapped", sizeof(fScalerWhy));
		goto refused;
	}
	/* are its registers there at all (clock and D-TZPC): two that must hold what is written */
	dpuWrite(fDma5, IDMA_SRC_OFFSET_REG, 0x00020004);
	dpuWrite(fDpp5, DPP_IMG_SIZE_REG, 0x00100020);
	a = dpuRead(fDma5, IDMA_SRC_OFFSET_REG);
	b = dpuRead(fDpp5, DPP_IMG_SIZE_REG);
	if (a != 0x00020004 || b != 0x00100020) {
		snprintf(fScalerWhy, sizeof(fScalerWhy), "channel 5's registers do not hold what is written (IDMA %#x, DPP %#x)", a, b);
		goto refused;
	}
	/*
	 * A row's length is SRC_SIZE's width, in pixels: the whole buffer is f_w x f_h, the picture IMG_SIZE
	 * of it at SRC_OFFSET (idma_reg_set_coordinates, cal_9830/dpp_reg.c:77-85). The stride registers
	 * count only with IDMA_PLANE_n_STRIDE_SEL set, which S-Boot leaves off (it has 0 in both, and
	 * SRC_SIZE 1440 x 3200 for its 5760 bytes a row). A run that looked for the row in them refused.
	 */
	if ((dpuRead(fDma, IDMA_STRIDE_REG) & (0xFu << 20)) != 0 || (dpuRead(fDma, IDMA_SRC_SIZE_REG) & 0xffff) * 4 != fRowBytes) {
		snprintf(fScalerWhy, sizeof(fScalerWhy), "S-Boot's channel is not what was expected: stride select %#x, size %#x for %u "
		    "bytes a row", dpuRead(fDma, IDMA_STRIDE_REG), dpuRead(fDma, IDMA_SRC_SIZE_REG), fRowBytes);
		goto refused;
	}

	/* IDMA 5: S-Boot's channel's settings (its format is CoreAnimation's byte order: the copy path copies as it is) */
	dpuWrite(fDma5, IDMA_ENABLE_REG, dpuRead(fDma, IDMA_ENABLE_REG) & 0xff000000u);        /* outstanding reads */
	dpuWrite(fDma5, IDMA_IRQ_REG, IDMA_IRQ_PENDING | (dpuRead(fDma, IDMA_IRQ_REG) & 0xffffu));
	for (unsigned i = 0; i < sizeof(idmaCopied) / sizeof(idmaCopied[0]); i++) {
		dpuWrite(fDma5, idmaCopied[i], dpuRead(fDma, idmaCopied[i]));
	}
	dpuWrite(fDma5, IDMA_SRC_OFFSET_REG, 0);
	dpuWrite(fDma5, IDMA_SRC_SIZE_REG, (h << 16) | w);       /* (the width a row: set again with every frame's surface) */
	dpuWrite(fDma5, IDMA_IMG_SIZE_REG, (h << 16) | w);
	dpuWrite(fDma5, IDMA_BASE_Y8_REG, (uint32_t) vinfo.v_physaddr);        /* (memory that is there, until a frame) */

	/* DPP 5: S-Boot's DPP's settings, then the scaler */
	dpuWrite(fDpp5, DPP_ENABLE_REG, dpuRead(fDppT, DPP_ENABLE_REG) & DPP_ENABLE_COPIED);
	dpuWrite(fDpp5, DPP_IRQ_REG, DPP_IRQ_PENDING | (dpuRead(fDppT, DPP_IRQ_REG) & 0xffffu));
	dpuWrite(fDpp5, DPP_IN_CON_REG, dpuRead(fDppT, DPP_IN_CON_REG));
	for (uint32_t r = 0x30; r <= 0x40; r += 4) {
		dpuWrite(fDpp5, r, dpuRead(fDppT, r));                  /* CSC */
	}
	dpuWrite(fDpp5, DPP_DYNAMIC_GATING_REG, dpuRead(fDppT, DPP_DYNAMIC_GATING_REG));
	dpuWrite(fDpp5, DPP_IMG_SIZE_REG, (h << 16) | w);
	dpuWrite(fDpp5, DPP_SCALED_IMG_SIZE_REG, (ph << 16) | pw);
	dpuWrite(fDpp5, DPP_MAIN_H_RATIO_REG, (uint32_t) (((uint64_t) w << 20) / pw));
	dpuWrite(fDpp5, DPP_MAIN_V_RATIO_REG, (uint32_t) (((uint64_t) h << 20) / ph));
	for (unsigned n = 0; n < 9; n++) {
		for (unsigned x = 0; x < 2; x++) {
			for (unsigned t = 0; t < 4; t++) {
				dpuWrite(fDpp5, DPP_V_COEF(n, t, x), (uint32_t) scalerBicubic[n][t] & 0x7ff);
			}
			for (unsigned t = 0; t < 8; t++) {
				dpuWrite(fDpp5, DPP_H_COEF(n, t, x), (uint32_t) (t >= 2 && t <= 5 ? scalerBicubic[n][t - 2] : 0) & 0x7ff);
			}
		}
	}
	for (unsigned i = 0; i < 4; i++) {
		dpuWrite(fDpp5, DPP_POSITION_REG(i), 0);
	}
	/* taken at once: the channel is in nobody's path */
	dpuWrite(fDma5, IDMA_ENABLE_REG, dpuRead(fDma5, IDMA_ENABLE_REG) | IDMA_SFR_UPDATE_FORCE);
	dpuWrite(fDpp5, DPP_ENABLE_REG, dpuRead(fDpp5, DPP_ENABLE_REG) | DPP_SFR_UPDATE_FORCE);
	fScalerReady = true;
	fbSay("fb_scaler: channel %u ready for window %d (S-Boot's channel %u): %ux%u -> %ux%u, ratios %#x %#x; "
	    "S-Boot's IDMA enable %#x irq %#x in %#x req %#x, DPP enable %#x in %#x\n", SCALER_CH, fWin, fScanCh, w, h, pw, ph,
	    dpuRead(fDpp5, DPP_MAIN_H_RATIO_REG), dpuRead(fDpp5, DPP_MAIN_V_RATIO_REG),
	    dpuRead(fDma, IDMA_ENABLE_REG), dpuRead(fDma, IDMA_IRQ_REG), dpuRead(fDma, IDMA_IN_CON_REG),
	    dpuRead(fDma, IDMA_IN_REQ_DEST_REG), dpuRead(fDppT, DPP_ENABLE_REG), dpuRead(fDppT, DPP_IN_CON_REG));
	fbSay("fb_scaler: channel %u now: IDMA enable %#x irq %#x in %#x size %#x req %#x, DPP enable %#x irq %#x config errors %#x\n",
	    SCALER_CH, dpuRead(fDma5, IDMA_ENABLE_REG), dpuRead(fDma5, IDMA_IRQ_REG), dpuRead(fDma5, IDMA_IN_CON_REG),
	    dpuRead(fDma5, IDMA_SRC_SIZE_REG), dpuRead(fDma5, IDMA_IN_REQ_DEST_REG), dpuRead(fDpp5, DPP_ENABLE_REG),
	    dpuRead(fDpp5, DPP_IRQ_REG), dpuRead(fDpp5, DPP_CFG_ERR_STATE_REG));
	return;
refused:
	fbSay("fb_scaler: not used: %s\n", fScalerWhy);
}

/* A swap the controller can show as it is: one full-screen layer, BGRA, in one piece of memory below 4 GB. */
bool
IOMobileFramebuffer::scalerFits(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	MiniIOSurface * s = surfaces[0];
	int32_t src[4], dst[4];

	if (!fScalerReady || fDisplayOff || s == NULL || swapWord(swap, SWAP_SURFACE(1)) != 0 ||
	    swapWord(swap, SWAP_SURFACE(2)) != 0 || swapWord(swap, SWAP_SURFACE(3)) != 0) {
		return false;
	}
	memcpy(src, swap + SWAP_SOURCE(0), sizeof(src));
	memcpy(dst, swap + SWAP_DEST(0), sizeof(dst));

	const char *why = s->phys == 0 ? "its memory is not in one piece" :
	    s->info.planeCount != 0 || s->info.bytesPerElement != 4 || s->info.pixelFormat != 0x42475241 ? "not BGRA" :
	    s->info.width != fWidth || s->info.height != fHeight ? "not the display's size" :
	    s->info.bytesPerRow % 16 != 0 || s->info.bytesPerRow / 4 < fWidth || s->info.bytesPerRow / 4 > 0x3fff ? "its rows" :
	    s->phys + s->info.offset + (uint64_t) s->info.bytesPerRow * fHeight > (1ULL << 32) ? "above 4 GB" :
	    src[0] != 0 || src[1] != 0 || src[2] != (int32_t) fWidth || src[3] != (int32_t) fHeight ? "a part of it" :
	    dst[0] != 0 || dst[1] != 0 || dst[2] != (int32_t) fWidth || dst[3] != (int32_t) fHeight ? "not the whole screen" : NULL;

	if (why != NULL) {
		static const char *said[8];
		unsigned i = 0;

		/* each reason once, with the surface it was found on */
		while (i < 8 && said[i] != NULL && said[i] != why) {
			i++;
		}
		if (i < 8 && said[i] == NULL) {
			said[i] = why;
			fbSay("fb_scaler: surface %u (%ux%u '%.4s', %u bytes a row, at %#llx) by the processor: %s\n", s->sid,
			    s->info.width, s->info.height, (const char *) &s->info.pixelFormat, s->info.bytesPerRow, s->phys, why);
		}
		return false;
	}
	return true;
}

/*
 * The frame asked for last time has been taken (DECON latches at the start of a frame): the surface
 * before it is the panel's no longer and goes back to its maker. false: the scaler was turned off.
 */
bool
IOMobileFramebuffer::scalerWait(void)
{
	uint32_t mask = DECON_SHADOW_GLOBAL | (1u << fWin), irq, dppIrq, cfg, inUse, pathInUse;
	uint64_t began = mach_absolute_time(), limit;
	bool taken;

	if (!fScanPending) {
		return true;
	}
	(void) limit;
	latchWait(mask, fScanStart);
	fStatScanWait += mach_absolute_time() - began;
	fScanPending = false;
	taken = (dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) == 0;
	if (taken) {
		fLatchTime = mach_absolute_time();
	}
	irq = dpuRead(fDma5, IDMA_IRQ_REG);
	dppIrq = dpuRead(fDpp5, DPP_IRQ_REG);
	cfg = dpuRead(fDpp5, DPP_CFG_ERR_STATE_REG);
	inUse = dpuRead(fDma5, IDMA_BASE_Y8_REG + IDMA_SHADOW);
	pathInUse = dpuRead(fDecon, DECON_SHADOW_REGS + DECON_DATA_PATH_1);
	if (fStatScan <= 3 || !taken || (irq & IDMA_IRQ_ERRORS) || (dppIrq & DPP_IRQ_CONFIG_ERROR) || (cfg & 0x1f)) {
		uint64_t since;

		absolutetime_to_nanoseconds(mach_absolute_time() - fScanStart, &since);
		fbSay("fb_scaler: frame %u %s after %llu us: IDMA irq %#x, base in use %#x (asked %#x); DPP irq %#x, config errors %#x; "
		    "channels in use %#x; DECON global %#x\n", fStatScan, taken ? "taken" : "NOT taken", since / 1000, irq, inUse,
		    fScanAsked, dppIrq, cfg, pathInUse, dpuRead(fDecon, DECON_GLOBAL_CONTROL));
	}
	if ((irq & IDMA_IRQ_ERRORS) || (dppIrq & DPP_IRQ_CONFIG_ERROR) || (cfg & 0x1f)) {
		char why[120];

		snprintf(why, sizeof(why), "channel %u reports an error (IDMA irq %#x, DPP irq %#x, config errors %#x)", SCALER_CH, irq,
		    dppIrq, cfg);
		scalerOff(why, true);
		return false;
	}
	if (!taken) {
		if (++fStatScanNotTaken >= 3) {
			scalerOff("the switch is not taken", true);
			return false;
		}
		return true;
	}
	if (fScanShown != NULL) {
		MiniIOSurfaceDisplayUse(fScanShown, -1);
		MiniIOSurfaceDrop(fScanShown);
	}
	fScanShown = fScanNext;
	fScanNext = NULL;
	fShownSid = fScanShown != NULL ? fScanShown->sid : 0;
	fNextSid = 0;
	return true;
}

/* A frame to the panel through the scaler: the channel's base set to the surface, latched with the next frame. */
void
IOMobileFramebuffer::scalerShow(MiniIOSurface * s)
{
	uint32_t base = (uint32_t) (s->phys + s->info.offset);

	if (!scalerWait()) {
		return;
	}
	dpuWrite(fDma5, IDMA_BASE_Y8_REG, base);
	dpuWrite(fDma5, IDMA_SRC_SIZE_REG, (fHeight << 16) | (s->info.bytesPerRow / 4));       /* a row: in pixels */
	if (!fScalerOn) {
		uint32_t paths1 = dpuRead(fDecon, DECON_DATA_PATH_1);

		/* in nobody's path yet: its own registers taken at once; the window's channel with the frame */
		dpuWrite(fDma5, IDMA_ENABLE_REG, dpuRead(fDma5, IDMA_ENABLE_REG) | IDMA_SFR_UPDATE_FORCE);
		dpuWrite(fDecon, DECON_DATA_PATH_1, (paths1 & ~(7u << (4 * fWin))) | ((uint32_t) SCALER_CH << (4 * fWin)));
		fbSay("fb_scaler: on: window %d reads channel %u from the next frame (surface %u at %#x, %u bytes a row, size %#x)\n",
		    fWin, SCALER_CH, s->sid, base, s->info.bytesPerRow, dpuRead(fDma5, IDMA_SRC_SIZE_REG));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | DECON_SHADOW_GLOBAL | (1u << fWin));
	if (fScanNext != NULL) {
		MiniIOSurfaceDisplayUse(fScanNext, -1);
		MiniIOSurfaceDrop(fScanNext);
	}
	/* held, and in use for its maker, until the frame after it is taken */
	fScanNext = MiniIOSurfaceCopy(s->sid);
	MiniIOSurfaceDisplayUse(fScanNext, 1);
	fNextSid = s->sid;
	fScanAsked = base;
	fScanStart = mach_absolute_time();
	fScanPending = true;
	fScalerOn = true;
	fStatScan++;
	/*
	 * The swap is over only once the panel reads the new frame: until the shadow update latches,
	 * the controller still reads the frame before it, and CoreAnimation -- told the swap is done --
	 * draws its next frame into exactly that surface (run 16: the wallpaper tore while swiping).
	 * Waiting here (at most a frame) is what makes the swap's completion mean "on the panel".
	 */
	(void) scalerWait();
}

/* Back to S-Boot's channel (its buffer already holds this frame); forever: not again this boot. */
void
IOMobileFramebuffer::scalerOff(const char * why, bool forever)
{
	if (forever && fScalerReady) {
		fScalerReady = false;
		strlcpy(fScalerWhy, why, sizeof(fScalerWhy));
		fbSay("fb_scaler: off for the rest of this boot: %s\n", why);
	}
	if (!fScalerOn) {
		return;
	}
	uint32_t paths1 = dpuRead(fDecon, DECON_DATA_PATH_1), mask = DECON_SHADOW_GLOBAL | (1u << fWin);
	uint64_t began = mach_absolute_time(), limit;

	dpuWrite(fDecon, DECON_DATA_PATH_1, (paths1 & ~(7u << (4 * fWin))) | (fScanCh << (4 * fWin)));
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | mask);
	(void) limit;
	latchWait(mask, began);
	if (fScanShown != NULL) {
		MiniIOSurfaceDisplayUse(fScanShown, -1);
		MiniIOSurfaceDrop(fScanShown);
		fScanShown = NULL;
	}
	if (fScanNext != NULL) {
		MiniIOSurfaceDisplayUse(fScanNext, -1);
		MiniIOSurfaceDrop(fScanNext);
		fScanNext = NULL;
	}
	fScanPending = false;
	fScalerOn = false;
	if (fStatScanCpu < 4 || forever) {
		fbSay("fb_scaler: window %d back on channel %u (%s)\n", fWin, fScanCh, why);
	}
}

void
IOMobileFramebuffer::teMeasure(void)
{
	uint64_t start, now, limit, busySince = 0, lastTe = 0, busyTotal = 0, busyMax = 0, teToBusy = 0;
	uint32_t te0, fc0, te, samples = 0, idleSamples = 0, busyRuns = 0, teToBusyN = 0, lineMax = 0, lineChanges = 0, lastLine = 0;
	uint32_t maskedFrames, maskedBusy = 0, maskedTe;
	bool idle = true;

	te0 = te = dpuRead(fDecon, DECON_TE_COUNT);
	fc0 = dpuRead(fDecon, DECON_FRAME_COUNT);
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &limit);
	start = mach_absolute_time();
	do {
		uint32_t global = dpuRead(fDecon, DECON_GLOBAL_CONTROL), count = dpuRead(fDecon, DECON_TE_COUNT);
		uint32_t line = (dpuRead(fDecon, DECON_OUTFIFO_COUNTER0) >> 16) & 0x3fff;
		bool nowIdle = (global & DECON_GLOBAL_IDLE) != 0;

		now = mach_absolute_time();
		samples++;
		if (count != te) {
			te = count;
			lastTe = now;
		}
		if (line != lastLine) {
			lineChanges++;
			lastLine = line;
		}
		lineMax = line > lineMax ? line : lineMax;
		idleSamples += nowIdle;
		if (idle && !nowIdle) {
			busyRuns++;
			busySince = now;
			if (lastTe != 0) {
				teToBusy += now - lastTe;
				teToBusyN++;
			}
		} else if (!idle && nowIdle && busySince != 0) {
			busyTotal += now - busySince;
			busyMax = now - busySince > busyMax ? now - busySince : busyMax;
		}
		idle = nowIdle;
	} while (now - start < limit);
	fbSay("fb: te: in 100 ms %u TE pulses, %u frames counted; %u samples, idle in %u; %u frames seen sent, %llu us each "
	    "(longest %llu), starting %llu us after their TE; output line counter changed %u times (highest %u)\n",
	    te - te0, dpuRead(fDecon, DECON_FRAME_COUNT) - fc0, samples, idleSamples, busyRuns,
	    busyRuns ? micros(busyTotal) / busyRuns : 0, micros(busyMax), teToBusyN ? micros(teToBusy) / teToBusyN : 0,
	    lineChanges, lineMax);
	fTeNeverIdle = busyRuns == 0;

	/* the trigger masked: does that stop the frames? */
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig | DECON_TRIG_MASK);
	(void) dpuRead(fDecon, DECON_TRIG_CONTROL);
	delay(20000);           /* (a frame that had begun is over) */
	maskedTe = dpuRead(fDecon, DECON_TE_COUNT);
	maskedFrames = dpuRead(fDecon, DECON_FRAME_COUNT);
	nanoseconds_to_absolutetime(40 * NSEC_PER_MSEC, &limit);
	start = mach_absolute_time();
	idle = true;
	do {
		bool nowIdle = (dpuRead(fDecon, DECON_GLOBAL_CONTROL) & DECON_GLOBAL_IDLE) != 0;

		maskedBusy += idle && !nowIdle;
		idle = nowIdle;
	} while (mach_absolute_time() - start < limit);
	maskedTe = dpuRead(fDecon, DECON_TE_COUNT) - maskedTe;
	maskedFrames = dpuRead(fDecon, DECON_FRAME_COUNT) - maskedFrames;
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig);
	fbSay("fb: te: with the trigger masked for 40 ms: %u TE pulses, %u frames counted, %u seen sent\n", maskedTe, maskedFrames,
	    maskedBusy);
	{
		/* the frame-start flag: cleared, is it clear, and set again once a frame has gone? */
		uint32_t cleared, later;

		dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START);
		cleared = dpuRead(fDecon, DECON_INT_PENDING);
		delay(40000);
		later = dpuRead(fDecon, DECON_INT_PENDING);
		fTeStartFlag = (cleared & DECON_INT_FRAME_START) == 0 && (later & DECON_INT_FRAME_START) != 0;
		fbSay("fb: te: frame-start flag: %#x after clearing, %#x 40 ms later: %s\n", cleared, later,
		    fTeStartFlag ? "used to catch late frames" : "not usable");
	}
	if (maskedBusy != 0 || (busyRuns == 0 && maskedFrames != 0 && maskedFrames == maskedTe)) {
		fTeNeverIdle = true;    /* (masking does not hold the frames back: copying under it does not help) */
		fbSay("fb: te: masking the trigger does not stop the frames\n");
	}
}

/* The frame-start interrupt: fb_irq. */
void
IOMobileFramebuffer::frameIrqSetup(void)
{
	uint32_t enable;

	if (fDecon == NULL) {
		fDecon = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DECON_F_PHYS, 0x10000);
	}
	if (fDecon == NULL) {
		fbSay("fb_irq: DECON_F not mapped: off\n");
		return;
	}
	enable = dpuRead(fDecon, DECON_INT_ENABLE);
	/* only frame start: frame done and the extra ones would want clearing of their own (decon_reg_set_int) */
	dpuWrite(fDecon, DECON_INT_ENABLE, (enable & ~(DECON_INT_FRAME_DONE | DECON_INT_EXTRA)) | DECON_INT_FRAME_START |
	    DECON_INT_EN_ALL);
	dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START | DECON_INT_FRAME_DONE | DECON_INT_EXTRA);
	if (gic_v3_spi_attach(DECON_INTID_FRAME_START, false, frameIrq, this) != KERN_SUCCESS) {
		dpuWrite(fDecon, DECON_INT_ENABLE, enable);
		fbSay("fb_irq: interrupt %u could not be attached: off\n", DECON_INTID_FRAME_START);
		return;
	}
	gic_v3_spi_mask(DECON_INTID_FRAME_START);
	fFrameIrq = true;
	fbSay("fb_irq: DECON frame start on interrupt %u (INTERRUPT_ENABLE %#x -> %#x): the waits for a frame to be taken sleep "
	    "until it\n", DECON_INTID_FRAME_START, enable, dpuRead(fDecon, DECON_INT_ENABLE));
}

/* (interrupt context) a frame has started: its flag cleared, the waiter woken, quiet until the next wait */
void
IOMobileFramebuffer::frameIrq(void * context, uint32_t intid)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) context;
	uint32_t pending = dpuRead(fb->fDecon, DECON_INT_PENDING);

	dpuWrite(fb->fDecon, DECON_INT_PENDING, pending & (DECON_INT_FRAME_START | DECON_INT_FRAME_DONE | DECON_INT_EXTRA));
	gic_v3_spi_mask(intid);
	if (pending & DECON_INT_FRAME_START) {
		OSIncrementAtomic((volatile SInt32 *) &fb->fFrameStarts);
	}
	thread_wakeup((event_t) (uintptr_t) &fb->fFrameStarts);
}

/*
 * Until the shadow bits of `mask` are clear (DECON has taken what was asked at `start`) or FLIP_LATCH_MS have
 * gone by. false: they did not clear.
 */
bool
IOMobileFramebuffer::latchWait(uint32_t mask, uint64_t start)
{
	uint64_t limit, cap;

	nanoseconds_to_absolutetime((uint64_t) FLIP_LATCH_MS * NSEC_PER_MSEC, &limit);
	nanoseconds_to_absolutetime((uint64_t) LATCH_CAP_MS * NSEC_PER_MSEC, &cap);
	while ((dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) != 0) {
		uint64_t now = mach_absolute_time();

		if (now - start >= limit) {
			fStatLatchLimit++;
			return false;
		}
		if (!fFrameIrq) {
			delay(100);
			continue;
		}
		uint64_t deadline = now + cap < start + limit ? now + cap : start + limit;
		uint32_t seen = fFrameStarts;

		dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START);   /* (no stale one) */
		assert_wait_deadline((event_t) (uintptr_t) &fFrameStarts, THREAD_UNINT, deadline);
		gic_v3_spi_unmask(DECON_INTID_FRAME_START);
		if ((dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) == 0) {
			clear_wait(current_thread(), THREAD_AWAKENED);
			gic_v3_spi_mask(DECON_INTID_FRAME_START);
			break;
		}
		if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT) {
			gic_v3_spi_mask(DECON_INTID_FRAME_START);
			fStatLatchCap++;
		} else if (fFrameStarts != seen) {
			fStatLatchIrq++;
		}
	}
	return true;
}

/* The frame-start flag for tePresent: a mark now, then whether a frame has started since. */
uint32_t
IOMobileFramebuffer::frameStartMark(void)
{
	/* (the presenter's thread, as latchWait: the interrupt is masked now, the flag is ours) */
	dpuWrite(fDecon, DECON_INT_PENDING, DECON_INT_FRAME_START);
	return fFrameStarts;
}

bool
IOMobileFramebuffer::frameStartSince(uint32_t mark)
{
	return fFrameStarts != mark ||
	       (dpuRead(fDecon, DECON_INT_PENDING) & DECON_INT_FRAME_START) != 0;
}

void
IOMobileFramebuffer::teSetup(void)
{
	uint32_t global, trig;

	if (fDecon == NULL) {
		fDecon = (volatile uint8_t *) ml_io_map((vm_offset_t) DPU_DECON_F_PHYS, 0x10000);
	}
	if (fDecon == NULL) {
		fbSay("fb: te: DECON_F not mapped: fb_te off\n");
		return;
	}
	global = dpuRead(fDecon, DECON_GLOBAL_CONTROL);
	trig = dpuRead(fDecon, DECON_TRIG_CONTROL);
	fbSay("fb: te: DECON global %#x trigger %#x (timer %#x, secure %#x), TE count %u, frames %u, output %#x, windows secure %#x, "
	    "interrupts %#x pending %#x\n", global, trig, dpuRead(fDecon, DECON_TRIG_TIMER), dpuRead(fDecon, DECON_TRIG_SECURE),
	    dpuRead(fDecon, DECON_TE_COUNT), dpuRead(fDecon, DECON_FRAME_COUNT), dpuRead(fDecon, DECON_OUTFIFO_COUNTER0),
	    dpuRead(fDecon, DECON_SECURE_CONTROL), dpuRead(fDecon, DECON_INT_ENABLE), dpuRead(fDecon, DECON_INT_PENDING));
	if (!(global & DECON_GLOBAL_CMD_MODE) || !(trig & DECON_TRIG_HW_EN) || (trig & DECON_TRIG_MASK)) {
		fbSay("fb: te: not a command-mode panel on an unmasked hardware trigger: fb_te off\n");
		return;
	}
	fTeTrig = trig;
	teMeasure();
	fBackSize = (size_t) fRowBytes * fPanelHeight;
	fBack = (uint8_t *) IOMallocData(fBackSize);
	if (fBack == NULL) {
		fbSay("fb: te: no memory for a back buffer of %zu bytes: fb_te off\n", fBackSize);
		return;
	}
	memcpy(fBack, fPanel, fBackSize);
	fBase = fBack;
	fTeCount0 = dpuRead(fDecon, DECON_TE_COUNT);
	fTeFrameCount0 = dpuRead(fDecon, DECON_FRAME_COUNT);
	fTe = true;
	fbSay("fb: te: on: frames drawn into a back buffer, copied to the panel's with the trigger masked%s\n",
	    fTeNeverIdle ? " (without waiting: IDLE never seen clear)" : ", once DECON is idle");
}

/* The rows a frame changed, from the back buffer to the panel's, while DECON is held from sending a frame. */
void
IOMobileFramebuffer::tePresent(int32_t y0, int32_t y1)
{
	uint64_t began = mach_absolute_time(), idleAt, limit, waited;
	uint32_t frames;
	RowJob job;

	y0 = y0 < 0 ? 0 : y0;
	y1 = y1 > (int32_t) fPanelHeight ? (int32_t) fPanelHeight : y1;
	if (y1 <= y0) {
		return;
	}
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig | DECON_TRIG_MASK);
	(void) dpuRead(fDecon, DECON_TRIG_CONTROL);
	nanoseconds_to_absolutetime(TE_IDLE_WAIT_MS * NSEC_PER_MSEC, &limit);
	for (unsigned attempt = 0;; attempt++) {
		if (!fTeNeverIdle) {
			while ((dpuRead(fDecon, DECON_GLOBAL_CONTROL) & DECON_GLOBAL_IDLE) == 0) {
				if (mach_absolute_time() - began > limit) {
					fTeTimeouts++;
					break;
				}
				delay(20);
			}
		}
		if (!fTeStartFlag) {
			delay(3);
			break;
		}
		/* a TE just before the mask may start a frame a moment later: idle, and no frame started since */
		uint32_t mark = frameStartMark();

		delay(TE_SETTLE_US);
		if (!frameStartSince(mark) || attempt == 2) {
			break;
		}
		fTeLate++;
	}
	idleAt = mach_absolute_time();
	frames = dpuRead(fDecon, DECON_FRAME_COUNT);
	uint32_t startsAtIdle = fFrameStarts;
	bzero(&job, sizeof(job));
	job.copy = true;
	job.ry0 = y0;
	job.ry1 = y1;
	runJob(&job);
	__asm__ volatile ("dsb sy" ::: "memory");
	fTeRaced += dpuRead(fDecon, DECON_FRAME_COUNT) != frames ||
	    (fTeStartFlag && frameStartSince(startsAtIdle));
	dpuWrite(fDecon, DECON_TRIG_CONTROL, fTeTrig);
	waited = idleAt - began;
	fTeFrames++;
	fTeRows += (uint64_t) (y1 - y0);
	fTeIdleWait += waited;
	fTeIdleWaitMax = waited > fTeIdleWaitMax ? waited : fTeIdleWaitMax;
	fTeCopy += mach_absolute_time() - idleAt;
}

/* fb_te, every five seconds with the other numbers (fLock held) */
void
IOMobileFramebuffer::teReport(uint64_t nanos)
{
	uint32_t te = dpuRead(fDecon, DECON_TE_COUNT), frames = dpuRead(fDecon, DECON_FRAME_COUNT);
	uint64_t ms = nanos / NSEC_PER_MSEC ? nanos / NSEC_PER_MSEC : 1;

	fbSay("fb: te: %u copies in %llu ms, %llu rows each; waited %llu us for DECON (longest %llu), copied in %llu us; %u raced a frame, "
	    "%u late frames waited out, %u waits timed out; frames sent %llu a second\n", fTeFrames, ms,
	    fTeFrames ? fTeRows / fTeFrames : 0, fTeFrames ? micros(fTeIdleWait) / fTeFrames : 0, micros(fTeIdleWaitMax),
	    fTeFrames ? micros(fTeCopy) / fTeFrames : 0, fTeRaced, fTeLate, fTeTimeouts, (uint64_t) (frames - fTeFrameCount0) * 1000 / ms);
	fTeCount0 = te;
	fTeFrameCount0 = frames;
	fTeFrames = fTeRaced = fTeTimeouts = fTeLate = 0;
	fTeRows = fTeIdleWait = fTeIdleWaitMax = fTeCopy = 0;
}

/* fb_bl: DSIM0 and what it is doing; the brightness is sent from a thread call (the requests come in bursts). */
void
IOMobileFramebuffer::blSetup(void)
{
	fDsim = (volatile uint8_t *) ml_io_map((vm_offset_t) DSIM0_PHYS, 0x1000);
	if (fDsim == NULL) {
		fbSay("fb: bl: DSIM0 not mapped\n");
		return;
	}
	fbSay("fb: bl: DSIM0 config %#x, command config %#x, FIFO %#x, link %#x, interrupts %#x%s\n", dpuRead(fDsim, DSIM_CONFIG),
	    dpuRead(fDsim, DSIM_CMD_CONFIG), dpuRead(fDsim, DSIM_FIFOCTRL), dpuRead(fDsim, DSIM_LINK_STATUS1),
	    dpuRead(fDsim, DSIM_INTSRC), gBL ? "" : "; brightness requests only logged (fb_bl=1 to send them)");
	fBLLock = IOLockAlloc();
	fDsimLock = IOLockAlloc();
	fDsimReads = true;
	if (gBL) {
		fBLCall = thread_call_allocate_with_options(&IOMobileFramebuffer::blWork, this, THREAD_CALL_PRIORITY_KERNEL,
		    THREAD_CALL_OPTIONS_ONCE);
		IOLockLock(fDsimLock);
		panelReport("as found");
		fAor0 = PANEL_AOR_BRIGHTEST;
		IOLockUnlock(fDsimLock);
		fbSay("fb: bl: brightness by AOR from %#x (full) to %#x (darkest)\n", fAor0, PANEL_AOR_DARKEST);
	}
}

/* the panel's AOR (B1, two bytes), or the brightest of Samsung's table if it cannot be read. With fDsimLock held. */
uint32_t
IOMobileFramebuffer::aorRead(void)
{
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 };
	uint8_t value[2];
	int got;

	(void) dsimWrite(keyOn, sizeof(keyOn));
	got = dsimRead(0xB1, value, 2);
	(void) dsimWrite(keyOff, sizeof(keyOff));
	return got == 2 ? (uint32_t) value[0] << 8 | value[1] : PANEL_AOR_BRIGHTEST;
}

/*
 * One DCS read: the largest packet the panel may answer with, then the read with the bus turned
 * around (BTA); the answer comes back in DSIM's RX FIFO (dsim_drv.c dsim_read_data). Bytes read, or
 * -1. With fDsimLock held.
 */
int
IOMobileFramebuffer::dsimRead(uint8_t command, uint8_t * out, uint32_t max)
{
	uint64_t began, limit;
	uint32_t word, got = 0;

	if (!fDsimReads) {
		return -1;
	}
	nanoseconds_to_absolutetime(30 * NSEC_PER_MSEC, &limit);
	while (!(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY)) {
		(void) dpuRead(fDsim, DSIM_RXFIFO);     /* (anything left from before) */
		if (++got > 64) {
			break;
		}
	}
	got = 0;
	dpuWrite(fDsim, DSIM_INTSRC, DSIM_INT_RX_DONE);
	dpuWrite(fDsim, DSIM_PKTHDR, DSI_MAX_RETURN_SIZE | (max & 0xff) << 8);
	dpuWrite(fDsim, DSIM_PKTHDR, DSIM_PKTHDR_BTA | DSI_DCS_READ | (uint32_t) command << 8);
	began = mach_absolute_time();
	while ((dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY) && !(dpuRead(fDsim, DSIM_INTSRC) & DSIM_INT_RX_DONE)) {
		if (mach_absolute_time() - began > limit) {
			fDsimReads = false;
			fbSay("fb: bl: DCS read %#x: no answer in 30 ms (FIFO %#x, interrupts %#x, link %#x): no more reads\n", command,
			    dpuRead(fDsim, DSIM_FIFOCTRL), dpuRead(fDsim, DSIM_INTSRC), dpuRead(fDsim, DSIM_LINK_STATUS1));
			return -1;
		}
		delay(10);
	}
	delay(20);
	for (unsigned words = 0; words < 8 && !(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_RX_EMPTY); words++) {
		word = dpuRead(fDsim, DSIM_RXFIFO);
		switch (word & 0xff) {
		case DSI_RX_SHORT_1:
			out[got++] = (uint8_t) (word >> 8);
			break;
		case DSI_RX_SHORT_2:
			out[got++] = (uint8_t) (word >> 8);
			if (got < max) {
				out[got++] = (uint8_t) (word >> 16);
			}
			break;
		case DSI_RX_LONG: {
			uint32_t length = (word >> 8) & 0xffff;

			for (uint32_t i = 0; i < length; i += 4) {
				uint32_t data = dpuRead(fDsim, DSIM_RXFIFO);

				for (uint32_t b = 0; b < 4 && i + b < length; b++) {
					if (got < max) {
						out[got++] = (uint8_t) (data >> (8 * b));
					}
				}
			}
			break;
		}
		case DSI_RX_ERROR_REPORT:
			fbSay("fb: bl: DCS read %#x: the panel reports error %#x\n", command, (word >> 8) & 0xffff);
			break;
		default:
			break;
		}
	}
	return (int) got;
}

/* What the panel says of itself: power mode (0A), CTRL display (54: what 53 was set to, C0 = HBM), brightness (6A), ID (04). */
void
IOMobileFramebuffer::panelReport(const char * when)
{
	static const struct { uint8_t command, bytes; const char * name; } reads[] = {
		{ 0x0A, 1, "power mode" }, { 0x54, 1, "CTRL display" }, { 0x6A, 3, "DBV" }, { 0xB1, 2, "AOR" }, { 0x04, 3, "ID" },
		{ 0xCA, 34, "gamma" },
	};
	char line[512];
	size_t at = (size_t) snprintf(line, sizeof(line), "fb: bl: panel %s:", when);
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 };

	(void) dsimWrite(keyOn, sizeof(keyOn));         /* (Samsung's own registers read 0 while locked) */
	for (unsigned i = 0; i < sizeof(reads) / sizeof(reads[0]) && at < sizeof(line); i++) {
		uint8_t value[40] = { 0 };
		int got = dsimRead(reads[i].command, value, reads[i].bytes);

		at += (size_t) snprintf(line + at, sizeof(line) - at, " %s %02x =", reads[i].name, reads[i].command);
		for (int b = 0; b < got && at < sizeof(line); b++) {
			at += (size_t) snprintf(line + at, sizeof(line) - at, " %02x", value[b]);
		}
		if (got <= 0 && at < sizeof(line)) {
			at += (size_t) snprintf(line + at, sizeof(line) - at, " (%s)", got < 0 ? "no answer" : "nothing");
		}
		if (got < 0) {
			break;
		}
	}
	(void) dsimWrite(keyOff, sizeof(keyOff));
	fbSay("%s\n", line);
}

/* One DCS write: up to two bytes a short packet, more a long one through the payload FIFO. */
bool
IOMobileFramebuffer::dsimWrite(const uint8_t * bytes, uint32_t length)
{
	uint64_t began = mach_absolute_time(), limit;
	uint32_t header;

	nanoseconds_to_absolutetime(50 * NSEC_PER_MSEC, &limit);
	while (dpuRead(fDsim, DSIM_FIFOCTRL) & (DSIM_FIFO_PH_FULL | DSIM_FIFO_PL_FULL)) {
		if (mach_absolute_time() - began > limit) {
			return false;
		}
		delay(10);
	}
	if (fPacketGo) {
		dpuWrite(fDsim, DSIM_CMD_CONFIG, dpuRead(fDsim, DSIM_CMD_CONFIG) | DSIM_CMD_PKT_GO_EN);
	}
	if (length > 2) {
		for (uint32_t i = 0; i < length; i += 4) {
			uint32_t word = 0;

			for (uint32_t b = 0; b < 4 && i + b < length; b++) {
				word |= (uint32_t) bytes[i + b] << (8 * b);
			}
			dpuWrite(fDsim, DSIM_PAYLOAD, word);
		}
		header = DSI_DCS_LONG_WRITE | (length & 0xff) << 8 | (length >> 8 & 0xff) << 16;
	} else {
		header = (length == 2 ? DSI_DCS_SHORT_WRITE_1 : DSI_DCS_SHORT_WRITE) | (uint32_t) bytes[0] << 8 |
		    (uint32_t) (length == 2 ? bytes[1] : 0) << 16;
	}
	dpuWrite(fDsim, DSIM_PKTHDR, header);
	if (fPacketGo) {
		dpuWrite(fDsim, DSIM_CMD_CONFIG, dpuRead(fDsim, DSIM_CMD_CONFIG) & ~DSIM_CMD_PKT_GO_EN);
	}
	while (!(dpuRead(fDsim, DSIM_FIFOCTRL) & DSIM_FIFO_PH_EMPTY)) {
		if (mach_absolute_time() - began > limit) {
			return false;
		}
		delay(10);
	}
	return true;
}

bool
IOMobileFramebuffer::blSend(uint32_t dbv, uint32_t aor)
{
	static const uint8_t keyOn[] = { 0xF0, 0x5A, 0x5A }, keyOff[] = { 0xF0, 0xA5, 0xA5 }, update[] = { 0xF7, 0x0F };
	static const uint8_t hbmOff[] = { 0x53, 0x00 };
	const uint8_t level[] = { 0x6A, (uint8_t) (dbv >> 8 & 0x07), (uint8_t) (dbv & 0xff), 0x01 };
	const uint8_t offRatio[] = { 0xB1, (uint8_t) (aor >> 8), (uint8_t) aor };
	bool ok;

	ok = dsimWrite(keyOn, sizeof(keyOn));
	ok = ok && dsimWrite(offRatio, sizeof(offRatio));
	ok = ok && dsimWrite(level, sizeof(level));
	ok = ok && dsimWrite(hbmOff, sizeof(hbmOff));
	ok = ok && dsimWrite(update, sizeof(update));
	return dsimWrite(keyOff, sizeof(keyOff)) && ok;
}

/* The framebuffer's power (IOMobileFramebuffer selector 12: 0 off, 1 on): the panel's display off / on (DCS 28 / 29). */
void
IOMobileFramebuffer::displayPower(bool on)
{
	const uint8_t command[] = { (uint8_t) (on ? 0x29 : 0x28) };
	bool ok;

	if (fDsim == NULL || !gBL || fDisplayOff == !on) {
		return;
	}
	IOLockLock(fDsimLock);
	ok = dsimWrite(command, sizeof(command));
	fDisplayOff = !on;
	IOLockUnlock(fDsimLock);
	fbSay("fb: bl: display %s (DCS %02x) %s\n", on ? "on" : "off", command[0], ok ? "sent" : "NOT SENT");
}

/* nits in 16.16 to the panel's DBV, between the steps of Samsung's table */
static uint32_t
nitsToDBV(uint32_t level)
{
	const unsigned steps = sizeof(gPanelNits) / sizeof(gPanelNits[0]);
	uint64_t milli = ((uint64_t) level * 1000) >> 16;

	if (level == 0) {
		return 0;
	}
	if (milli <= (uint64_t) gPanelNits[0] * 1000) {
		return gPanelDBV[0];
	}
	for (unsigned i = 0; i + 1 < steps; i++) {
		uint64_t lo = (uint64_t) gPanelNits[i] * 1000, hi = (uint64_t) gPanelNits[i + 1] * 1000;

		if (milli < hi) {
			return gPanelDBV[i] + (uint32_t) ((uint64_t) (gPanelDBV[i + 1] - gPanelDBV[i]) * (milli - lo) / (hi - lo));
		}
	}
	return gPanelDBV[steps - 1];
}

void
IOMobileFramebuffer::blWork(thread_call_param_t self, thread_call_param_t unused __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;

	for (;;) {
		uint32_t target, dbv, aor, back;
		uint64_t began, nits;
		bool ok;

		IOLockLock(fb->fBLLock);
		target = fb->fBLTarget;
		IOLockUnlock(fb->fBLLock);
		if (gBLThermalCap != 0 && target > gBLThermalCap) {
			target = gBLThermalCap;
		}
		dbv = nitsToDBV(target);
		nits = (uint64_t) target < ((uint64_t) kPanelMaxNits << 16) ? target : (uint64_t) kPanelMaxNits << 16;
		aor = fb->fAor0 < PANEL_AOR_DARKEST ?
		    PANEL_AOR_DARKEST - (uint32_t) ((uint64_t) (PANEL_AOR_DARKEST - fb->fAor0) * nits / ((uint64_t) kPanelMaxNits << 16)) :
		    fb->fAor0;
		if (aor == fb->fBLSent && fb->fBLWrites != 0) {
			return;
		}
		began = mach_absolute_time();
		IOLockLock(fb->fDsimLock);
		ok = fb->blSend(dbv, aor);
		fb->fBLSent = aor;
		fb->fBLWrites++;
		fb->fBLFailures += !ok;
		if (fb->fBLWrites <= 2) {
			char when[32];

			snprintf(when, sizeof(when), "after write %u", fb->fBLWrites);
			fb->panelReport(when);
		}
		back = fb->fBLWrites <= 2 ? fb->aorRead() : aor;
		IOLockUnlock(fb->fDsimLock);
		if (fb->fBLWrites <= 40 || !ok || fb->fBLWrites % 100 == 0) {
			fbSay("fb: bl: %u.%03u nits -> AOR %#x (read back %#x%s), DBV %#x %s in %llu us (write %u, %u failed)\n", target >> 16,
			    (uint32_t) (((uint64_t) (target & 0xffff) * 1000) >> 16), aor, back, fb->fPacketGo ? ", PKT_GO" : "", dbv,
			    ok ? "sent" : "NOT SENT", micros(mach_absolute_time() - began), fb->fBLWrites, fb->fBLFailures);
		}
	}
}

/* CoreBrightness: IORegistryEntrySetCFProperty(the framebuffer, "IOMFBBrightnessLevel", nits in 16.16) */
IOReturn
IOMobileFramebuffer::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSNumber * level;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	level = OSDynamicCast(OSNumber, dict->getObject("IOMFBBrightnessLevel"));
	fBLRequests++;
	if (fBLRequests <= 12 || (level == NULL && fBLRequests <= 64)) {
		OSCollectionIterator * keys = OSCollectionIterator::withCollection(dict);
		char names[256] = "";

		while (keys != NULL) {
			OSSymbol * key = OSDynamicCast(OSSymbol, keys->getNextObject());

			if (key == NULL) {
				break;
			}
			strlcat(names, " ", sizeof(names));
			strlcat(names, key->getCStringNoCopy(), sizeof(names));
		}
		OSSafeReleaseNULL(keys);
		fbSay("fb: bl: setProperties %u:%s%s%u\n", fBLRequests, names, level ? " = " : "", level ? level->unsigned32BitValue() : 0);
	}
	if (level == NULL) {
		return kIOReturnUnsupported;
	}
	brightnessRequest(level->unsigned32BitValue());
	return kIOReturnSuccess;
}

/* a brightness, in nits 16.16, for the panel: sent from the thread call (requests come in bursts: the last one wins) */
void
IOMobileFramebuffer::brightnessRequest(uint32_t level)
{
	if (fBLCall == NULL || fDsim == NULL) {
		return;
	}
	IOLockLock(fBLLock);
	fBLTarget = level;
	IOLockUnlock(fBLLock);
	thread_call_enter(fBLCall);
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->ry1 - job->ry0;
		y0 = job->ry0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->ry0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst, bool damage)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (filter && fBlendRowSize < (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.filter = filter;
	job.copy = false;
	job.compare = false;
	job.changed = NULL;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	job.ry0 = py0;
	job.ry1 = py1;
	if (damage && gDamage && !fDiff) {
		uint64_t compared = mach_absolute_time();
		bool changed = damageRows(&job, &job.ry0, &job.ry1);

		fStatCompare += mach_absolute_time() - compared;
		if (!changed) {
			fStatSkipped++;
			fStatRowsFull += (uint64_t) (py1 - py0);
			return;
		}
	} else {
		fShadowValid = false;
	}
	if (fFlip) {
		/* this buffer was last drawn two frames ago: what the frame in between changed is missing here too */
		int32_t own0 = job.ry0, own1 = job.ry1;
		int32_t p0 = fPrevRy0 > py0 ? fPrevRy0 : py0, p1 = fPrevRy1 < py1 ? fPrevRy1 : py1;

		if (p1 > p0) {
			job.ry0 = p0 < job.ry0 ? p0 : job.ry0;
			job.ry1 = p1 > job.ry1 ? p1 : job.ry1;
		}
		fOwnRy0 = own0 < fOwnRy0 ? own0 : fOwnRy0;
		fOwnRy1 = own1 > fOwnRy1 ? own1 : fOwnRy1;
		fFrameDrew = true;
	} else if (fTe) {
		fOwnRy0 = job.ry0 < fOwnRy0 ? job.ry0 : fOwnRy0;
		fOwnRy1 = job.ry1 > fOwnRy1 ? job.ry1 : fOwnRy1;
		fFrameDrew = true;
	}
	fStatRows += (uint64_t) (job.ry1 - job.ry0);
	fStatRowsFull += (uint64_t) (py1 - py0);
	{
		uint64_t scaled = mach_absolute_time();

		runJob(&job);
		fStatScale += mach_absolute_time() - scaled;
	}
}

/* A job's rows drawn: by this thread alone, or shared out (fb_threads) with this thread taking the first share. */
void
IOMobileFramebuffer::runJob(RowJob * job)
{
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads || job->ry1 - job->ry0 < 64) {
		drawRows(job, job->ry0, job->ry1, fBlendRow);
		return;
	}
	IOLockLock(fHelperLock);
	fHelperJob = job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(job, job->ry0, job->ry0 + (int32_t) ((int64_t) (job->ry1 - job->ry0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * fb_damage: which panel rows of this layer need drawing. Compares each row of the picture with
 * the copy of what was last shown (fShadow), keeps the rows that changed, and works out the panel
 * rows that take anything from them: with filtering a panel row mixes two picture rows, the one
 * its sample falls in and the next. False: nothing changed, nothing to draw. Anything else about
 * the layer that changed (its rectangles, the filter) makes it a whole frame, as before.
 */
typedef uint64_t __attribute__((aligned(4), may_alias)) word64_t;
typedef uint32_t __attribute__((aligned(4), may_alias)) word32_t;

/*
 * Whether two rows of pixels differ: eight bytes at a time, four of those to a test. (The kernel's
 * memcmp() goes byte by byte -- 11 million steps for a frame, more than all the drawing it saves.)
 */
static bool
rowsDiffer(const uint8_t * a, const uint8_t * b, size_t bytes)
{
	const word64_t * x = (const word64_t *) a;
	const word64_t * y = (const word64_t *) b;
	size_t words = bytes / 8, i = 0;

	for (; i + 4 <= words; i += 4) {
		if (((x[i] ^ y[i]) | (x[i + 1] ^ y[i + 1]) | (x[i + 2] ^ y[i + 2]) | (x[i + 3] ^ y[i + 3])) != 0) {
			return true;
		}
	}
	for (; i < words; i++) {
		if (x[i] != y[i]) {
			return true;
		}
	}
	if (bytes & 4) {
		return *(const word32_t *) (a + bytes - 4) != *(const word32_t *) (b + bytes - 4);
	}
	return false;
}

bool
IOMobileFramebuffer::damageRows(const RowJob * job, int32_t * ry0, int32_t * ry1)
{
	int32_t key[10] = { job->sx, job->sy, job->sw, job->sh, job->px0, job->py0, job->px1, job->py1, job->filter, (int32_t) fPanelWidth };
	size_t rowBytes = (size_t) job->sw * 4, need = rowBytes * (size_t) job->sh;
	int32_t changedFirst = -1, changedLast = -1, first = -1, last = -1;

	if (need > fShadowSize) {
		if (fShadow != NULL) {
			IOFree(fShadow, fShadowSize);
		}
		fShadow = (uint8_t *) IOMalloc(need);
		fShadowSize = fShadow != NULL ? need : 0;
		fShadowValid = false;
	}
	if (fShadow == NULL) {
		return true;            /* no room for the copy: draw it all */
	}
	if (!fShadowValid || memcmp(key, fShadowKey, sizeof(key)) != 0) {
		for (int32_t r = 0; r < job->sh; r++) {
			memcpy(fShadow + (size_t) r * rowBytes, job->pixels + (size_t) (job->sy + r) * job->stride + (size_t) job->sx * 4, rowBytes);
		}
		memcpy(fShadowKey, key, sizeof(key));
		fShadowValid = true;
		return true;            /* the whole layer: ry0..ry1 as given */
	}
	{
		/* the rows compared by every one who draws, each a share of them (fb_threads) */
		int32_t changed[2] = { INT32_MAX, -1 };
		RowJob compare = *job;

		compare.compare = true;
		compare.copy = false;
		compare.ry0 = 0;
		compare.ry1 = job->sh;
		compare.changed = changed;
		runJob(&compare);
		if (changed[1] < 0) {
			return false;
		}
		changedFirst = changed[0];
		changedLast = changed[1];
	}
	if (changedFirst < 0) {
		return false;
	}
	/* the panel rows that take anything from picture rows changedFirst..changedLast */
	for (int32_t y = job->py0; y < job->py1; y++) {
		int32_t lo, hi;

		if (job->unscaled) {
			lo = hi = y - job->py0;
		} else if (job->filter) {
			int64_t at = sample_at(y - job->py0, job->sh, job->py1 - job->py0);

			lo = (int32_t) (at >> 8);
			hi = (at & 255) != 0 && lo + 1 < job->sh ? lo + 1 : lo;
		} else {
			lo = hi = (int32_t) ((int64_t) (y - job->py0) * job->sh / (job->py1 - job->py0));
		}
		if (hi >= changedFirst && lo <= changedLast) {
			first = first < 0 ? y : first;
			last = y;
		}
	}
	if (first < 0) {
		return false;
	}
	*ry0 = first;
	*ry1 = last + 1;
	return true;
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		fbSay("fb: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;
	uint32_t sid0 = swapWord(swap, SWAP_SURFACE(0));

	if (gScaler && fScalerReady && sid0 != 0) {
		bool busy = sid0 == fShownSid || sid0 == fNextSid;

		for (unsigned k = 0; k < fSwapsWaiting; k++) {
			busy |= swapWord(fSwap[(fSwapFirst + k) % SWAP_QUEUE], SWAP_SURFACE(0)) == sid0;
		}
		fStatIntoShown += busy;
	}
	fQueuedSwapID = swapWord(swap, SWAP_ID);
	fStatSwapSame += memcmp(fPrevSwapLayers, swap + SWAP_SURFACE(0), sizeof(fPrevSwapLayers)) == 0;
	memcpy(fPrevSwapLayers, swap + SWAP_SURFACE(0), sizeof(fPrevSwapLayers));
	{
		unsigned i;

		for (i = 0; i < 8 && fStatSwapSids[i] != 0 && fStatSwapSids[i] != sid0; i++) {
		}
		if (i == 8) {
			fStatSwapSidsMore++;
		} else {
			fStatSwapSids[i] = sid0;
		}
	}
	fStatSwapPid = proc_selfpid();
	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	/* in use from the moment it is queued, as Apple's driver counts it: a surface asked for is not free */
	if (gScaler && fScalerReady && fSwapSurfaces[slot][0] != NULL) {
		MiniIOSurfaceDisplayUse(fSwapSurfaces[slot][0], 1);
		fSwapQueuedUse[slot] = true;
	}
	uint32_t ticket = ++fSwapsQueued;
	IOLockWakeup(fSwapLock, fSwap, true);
	if (gScaler && gSwapWait && fScalerReady) {
		uint64_t until;

		clock_interval_to_deadline(100, kMillisecondScale, &until);
		while ((int32_t) (fSwapsDone - ticket) < 0) {
			if (IOLockSleepDeadline(fSwapLock, &fSwapsDone, until, THREAD_UNINT) == THREAD_TIMED_OUT) {
				fStatSwapWaitTimeouts++;
				break;
			}
		}
	}
	IOLockUnlock(fSwapLock);
}

/*
 * IOMobileFramebufferSwapWait: until swap `id` is on the panel (see gSwapWait), or it was never queued, or
 * nothing is left on its way to the panel (a swap the presenter gave up on), at most 100 ms.
 */
void
IOMobileFramebuffer::swapWait(uint32_t id)
{
	uint64_t began = mach_absolute_time(), until;
	bool blocked = false;

	clock_interval_to_deadline(100, kMillisecondScale, &until);
	IOLockLock(fSwapLock);
	fStatSwapWaits++;
	while ((int32_t) (fShownSwapID - id) < 0 && (int32_t) (fQueuedSwapID - id) >= 0 &&
	    (fSwapsWaiting != 0 || fSwapsDone != fSwapsQueued)) {
		blocked = true;
		if (IOLockSleepDeadline(fSwapLock, &fShownSwapID, until, THREAD_UNINT) == THREAD_TIMED_OUT) {
			fStatSwapWaitTimeouts2++;
			break;
		}
	}
	if (blocked) {
		fStatSwapWaitsBlocked++;
		fStatSwapWaitTime += mach_absolute_time() - began;
	}
	IOLockUnlock(fSwapLock);
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		bool queuedUse = fSwapQueuedUse[fSwapFirst];

		fSwapQueuedUse[fSwapFirst] = false;
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		if (queuedUse && surfaces[0] != NULL) {
			MiniIOSurfaceDisplayUse(surfaces[0], -1);       /* (the scaler holds its own while it shows it) */
		}
		IOLockLock(fSwapLock);
		fSwapsDone++;
		fShownSwapID = swapWord(swap, SWAP_ID);
		IOLockWakeup(fSwapLock, &fSwapsDone, false);
		IOLockWakeup(fSwapLock, &fShownSwapID, false);
		IOLockUnlock(fSwapLock);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool report;

	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		if (surfaces[layer] != NULL && !MiniIOSurfaceDrawn(surfaces[layer], SWAP_GPU_MS)) {
			IOLog("MiniIOMobileFramebuffer: swap %u: the GPU is not done with surface %u after %u ms; shown as it is\n",
			    swapWord(swap, SWAP_ID), swapWord(swap, SWAP_SURFACE(layer)), SWAP_GPU_MS);
		}
	}
	fStatWaiting += mach_absolute_time() - began;
	began = ready = mach_absolute_time();
	if (gOnTheBeat) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	if (fFlip) {
		flipWait();
	} else if (fFlipWanted && fGraphics) {
		flipEnable();
	}
	fOwnRy0 = INT32_MAX;
	fOwnRy1 = -1;
	fFrameDrew = false;
	/* its turn has come: the swap after it may be handed over */
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	bool zeroCopy = false;

	if (gScaler && scalerFits(swap, surfaces)) {
		scalerShow(surfaces[0]);
		zeroCopy = fScalerOn;
	}
	if (zeroCopy) {
		if (!fGraphics) {
			fGraphics = true;
			PE_initialize_console(NULL, kPEGraphicsMode);
		}
		layers = 1;
		recorded = surfaces[0];
	} else if (fScalerOn) {
		fShadowValid = false;           /* the panel's own buffer is frames behind: all of it again, then back to it */
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS && !zeroCopy; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				fShadowValid = false;
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
				fOwnRy0 = 0;
				fOwnRy1 = (int32_t) fPanelHeight;
				fFrameDrew = true;
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			/* damage only where nothing is drawn over the bottom layer: above it would be drawn over */
			/* ... in this swap and in the one before (what was drawn over it then is still on the panel) */
			drawLayer(s, src, dst, layer == 0 && (fLayersShown & ~1u) == 0 && swapWord(swap, SWAP_SURFACE(1)) == 0 &&
			    swapWord(swap, SWAP_SURFACE(2)) == 0 && swapWord(swap, SWAP_SURFACE(3)) == 0);
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	drawn = mach_absolute_time();
	if (fFlip && fFrameDrew) {
		flipNow();
	} else if (fTe && fFrameDrew) {
		tePresent(fOwnRy0, fOwnRy1);
		drawn = mach_absolute_time();
	}
	if (gScaler && !zeroCopy) {
		fStatScanCpu++;
		if (fScalerOn) {
			scalerOff("a frame the scaler cannot show", false);
		}
	}
	if (recorded != NULL) {
		record(recorded);
	}
	fSwapsShown++;
	if (gTouchLat) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		IOLog("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fbSay("fb: %u frames in %llu ms (%llu a second), %llu us each in the driver (comparing %llu, scaling %llu); %llu%% of the rows drawn\n",
		    fStatFrames, nanos / NSEC_PER_MSEC, fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    micros(fStatCompare) / fStatFrames, micros(fStatScale) / fStatFrames, fStatRowsFull ? fStatRows * 100 / fStatRowsFull : 0);
		fStatCompare = fStatScale = 0;
		if (gScaler) {
			fbSay("fb_scaler: %u frames read by the controller itself, %u drawn by the processor; %llu us waited for the switch a "
			    "frame; %u not taken%s%s\n", fStatScan, fStatScanCpu, fStatScan ? micros(fStatScanWait) / fStatScan : 0,
			    fStatScanNotTaken, fScalerReady ? "" : "; off: ", fScalerReady ? "" : fScalerWhy);
			fbSay("fb_swap: SwapEnd %s; %u SwapWaits, %u of them blocked (%llu us each), %u timed out; %u swaps found their "
			    "surface still on the panel or queued (would tear)\n", gSwapWait ? "waits for the panel (fb_swapwait=1)" :
			    "returns once queued", fStatSwapWaits, fStatSwapWaitsBlocked,
			    fStatSwapWaitsBlocked ? micros(fStatSwapWaitTime) / fStatSwapWaitsBlocked : 0, fStatSwapWaitTimeouts2,
			    fStatIntoShown);
			fbSay("fb_vsync: refresh notifications asked for %u times so far; %u sent, %u stopped (no port or callback)\n",
			    gVSyncAsked, gVSyncSent, gVSyncStopped);
			fbSay("fb_swap: by pid %d; %u swaps the same as the one before (surfaces, rectangles, layers); the bottom "
			    "layer's surfaces %u %u %u %u %u %u %u %u (+%u more)\n", fStatSwapPid, fStatSwapSame, fStatSwapSids[0],
			    fStatSwapSids[1], fStatSwapSids[2], fStatSwapSids[3], fStatSwapSids[4], fStatSwapSids[5],
			    fStatSwapSids[6], fStatSwapSids[7], fStatSwapSidsMore);
			fStatSwapSame = fStatSwapSidsMore = 0;
			bzero(fStatSwapSids, sizeof(fStatSwapSids));
			fbSay("fb_irq: waits for the controller to take a frame: %u ended by its frame-start interrupt, %u looked again "
			    "after %u ms, %u gave up after %u ms%s\n", fStatLatchIrq, fStatLatchCap, LATCH_CAP_MS, fStatLatchLimit,
			    FLIP_LATCH_MS, fFrameIrq ? "" : " (fb_irq off: looked every 100 us)");
			fStatLatchIrq = fStatLatchCap = fStatLatchLimit = 0;
			fStatScan = fStatScanCpu = 0;
			fStatScanWait = 0;
			fStatSwapWaits = fStatSwapWaitsBlocked = fStatSwapWaitTimeouts2 = fStatIntoShown = 0;
			fStatSwapWaitTime = 0;
		}
		if (fTe) {
			teReport(nanos);
		}
		fStatSwapsLate = 0;
		if (gDamage && fStatRowsFull != 0) {
			IOLog("MiniIOMobileFramebuffer: fb_damage: %llu%% of the panel's rows drawn, %u frames had no change\n",
			    fStatRows * 100 / fStatRowsFull, fStatSkipped);
		}
		fStatRows = fStatRowsFull = 0;
		fStatSkipped = 0;
		if (gFlip && !fFlip) {
			IOLog("MiniIOMobileFramebuffer: fb_flip asked for and off: %s\n", fDpuWhy[0] ? fDpuWhy : "not started yet");
		}
		if (fDpuDump[0] && fStatSince != 0 && fSwapsShown < 2000) {
			IOLog("MiniIOMobileFramebuffer: dpu: %s\n", fDpuDump);
		}
		if (fFlip) {
			uint64_t flipWaited;

			absolutetime_to_nanoseconds(fStatFlipWait, &flipWaited);
			IOLog("MiniIOMobileFramebuffer: fb_flip: %u flips so far, %llu us waited for the switch before drawing (%u not taken)\n",
			    fStatFlips, fStatFlips ? flipWaited / NSEC_PER_USEC / (fStatFrames ? fStatFrames : 1) : 0, fStatFlipLate);
			fStatFlipWait = 0;
		}
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	/*
	 * In step with the panel when the controller reads the frames itself: a vsync just after a frame was
	 * taken. A swap waits for its frame to be taken (fb_swapwait); a vsync out of phase with that makes
	 * the window server miss every other one (run 18: 33-35 frames a second, 6-7.5 ms waited a frame).
	 */
	uint64_t latch = fOwner != NULL ? fOwner->fLatchTime : 0, slack;

	nanoseconds_to_absolutetime(300 * NSEC_PER_USEC, &slack);
	if (latch != 0 && now > latch && now - latch < 120 * period) {
		uint64_t next = latch + slack + ((now - latch) / period + 1) * period;

		if (next > now && next <= now + period + slack) {
			fVSyncTime = next;
		}
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/*
 * One refresh: tell the client, which gets three 64-bit numbers as six 32-bit halves --
 * the time of this refresh, the same again (the time it was presented), and a counter.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref;
	io_user_reference_t args[6];
	uint64_t now;
	bool send = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (fClosed || fCallbacks[kFBNotifyVSync] == 0 || fPorts[kFBNotifyVSync] == NULL) {
		fVSyncRunning = false;
		OSIncrementAtomic(&gVSyncStopped);
	} else {
		OSIncrementAtomic(&gVSyncSent);
		fVSyncCount++;
		setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
		args[0] = (uint32_t) now;
		args[1] = now >> 32;
		args[2] = (uint32_t) now;
		args[3] = now >> 32;
		args[4] = (uint32_t) fVSyncCount;
		args[5] = fVSyncCount >> 32;
		send = true;
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
		if (gTouchLat) {
			/* the refresh it is for, and when it was sent: tools/frame_split.py's refresh -> swap */
			redwood_tl("tl: vs %llu due %llu sent %llu\n", fVSyncCount, micros(now), micros(mach_absolute_time()));
		}
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		/* into the loader's log (fbSay): the message buffer wraps before anybody reads the first calls */
		fbSay("fb: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* in: the swap; returns once it is on the panel */
		if (gTouchLat) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		if (!gSwapWait && args->scalarInputCount > 0) {
			fOwner->swapWait((uint32_t) in[0]);
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:               /* in: 0 off, 1 on */
		if (args->scalarInputCount >= 1) {
			fOwner->displayPower(in[0] != 0);
		}
		return kIOReturnSuccess;

	case kFBMethodChangeFrameInfo:
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (in[2] == kFBNotifyVSync) {
			OSIncrementAtomic(&gVSyncAsked);
		}
		if (in[2] == kFBNotifyVSync && in[0] != 0 && !fVSyncRunning && !fClosed) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: type; out: how many, and their tags: none */
		if (args->scalarOutputCount >= 1) {
			out[0] = 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
		}
		return kIOReturnSuccess;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		IOLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

/*
 * The display's backlight, as CoreBrightness (backboardd) looks for it: a service with
 * "backlight-control" = true that conforms to AppleARMBacklight (CBDisplayContaineriOS). Only for
 * such a service does it make its display module, which then sets the brightness -- as
 * "IOMFBBrightnessLevel" (nits, 16.16) on the framebuffer, and by IORegistryEntrySetCFProperties on
 * this service. It reads "IODisplayParameters" = { brightness = { min, max, value } } from here.
 * Everything it sets here is said (fb: bl: lines) and a brightness taken from it goes to the panel
 * like the framebuffer's: value / max of the panel's 420 nits.
 */

class AppleARMBacklight : public IOService
{
	OSDeclareDefaultStructors(AppleARMBacklight);

public:
	IOMobileFramebuffer *   fFB;
	uint32_t                fValue;
	uint32_t                fRequests;

	void publishParameters(void);
	bool takeBrightness(OSObject * value, const char * name);
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleARMBacklight, IOService);

void
AppleARMBacklight::publishParameters(void)
{
	OSDictionary * parameters = OSDictionary::withCapacity(1);
	OSDictionary * brightness = OSDictionary::withCapacity(3);

	if (parameters != NULL && brightness != NULL) {
		setNumber(brightness, "min", 0);
		setNumber(brightness, "max", kBacklightMax);
		setNumber(brightness, "value", fValue);
		parameters->setObject("brightness", brightness);
		setProperty("IODisplayParameters", parameters);
	}
	OSSafeReleaseNULL(brightness);
	OSSafeReleaseNULL(parameters);
}

/* a "brightness": a number of 0..max, or { value = N } */
bool
AppleARMBacklight::takeBrightness(OSObject * value, const char * name)
{
	OSNumber * number = OSDynamicCast(OSNumber, value);
	OSDictionary * dict = OSDynamicCast(OSDictionary, value);

	if (number == NULL && dict != NULL) {
		number = OSDynamicCast(OSNumber, dict->getObject("value"));
	}
	if (number == NULL) {
		return false;
	}
	fValue = number->unsigned32BitValue() > kBacklightMax ? kBacklightMax : number->unsigned32BitValue();
	publishParameters();
	if (fFB != NULL) {
		fFB->brightnessRequest((uint32_t) ((uint64_t) fValue * kPanelMaxNits));     /* value / max * 420 nits, in 16.16 */
	}
	fbSay("fb: bl: backlight %s = %u of %u\n", name, fValue, kBacklightMax);
	return true;
}

IOReturn
AppleARMBacklight::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSSymbol * key;
	bool taken = false;

	if (dict == NULL) {
		return kIOReturnBadArgument;
	}
	fRequests++;
	keys = OSCollectionIterator::withCollection(dict);
	while (keys != NULL && (key = OSDynamicCast(OSSymbol, keys->getNextObject())) != NULL) {
		OSObject * value = dict->getObject(key);
		OSNumber * number = OSDynamicCast(OSNumber, value);

		if (fRequests <= 40) {
			fbSay("fb: bl: backlight setProperties %u: %s = %s%llu\n", fRequests, key->getCStringNoCopy(),
			    number ? "" : value && OSDynamicCast(OSDictionary, value) ? "(dictionary) " : "(other) ",
			    number ? number->unsigned64BitValue() : 0);
		}
		if (key->isEqualTo("brightness")) {
			taken |= takeBrightness(value, "brightness");
		} else if (key->isEqualTo("IODisplayParameters") && OSDynamicCast(OSDictionary, value) != NULL) {
			OSObject * inner = ((OSDictionary *) value)->getObject("brightness");

			taken |= inner != NULL && takeBrightness(inner, "IODisplayParameters.brightness");
		}
	}
	OSSafeReleaseNULL(keys);
	return taken ? kIOReturnSuccess : kIOReturnUnsupported;
}

static void
publishBacklight(IOMobileFramebuffer * fb)
{
	AppleARMBacklight * backlight = OSTypeAlloc(AppleARMBacklight);

	if (backlight != NULL && backlight->init() && backlight->attach(fb)) {
		backlight->fFB = fb;
		backlight->fValue = kBacklightMax / 2;
		backlight->setName("backlight");
		backlight->setProperty("backlight-control", kOSBooleanTrue);
		backlight->publishParameters();
		backlight->registerService();
		fbSay("fb: bl: AppleARMBacklight published%s\n", gBL ? "" : " (fb_bl=0: requests only logged)");
	}
	OSSafeReleaseNULL(backlight);
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
/*
 * On the way back to the loader (osfmk/arm64/loader_return.c, one processor, interrupts off): the panel's
 * window back on S-Boot's channel, so the loader's menu -- drawn into the firmware's framebuffer -- is what
 * shows, not CoreAnimation's last frame on the scaler's channel. Registers only: no lock, no sleep.
 */
void
IOMobileFramebuffer::loaderQuiesce(void)
{
	if (!fScalerOn || fDecon == NULL || fWin < 0) {
		return;
	}
	uint32_t paths1 = dpuRead(fDecon, DECON_DATA_PATH_1), mask = DECON_SHADOW_GLOBAL | (1u << fWin);

	dpuWrite(fDecon, DECON_DATA_PATH_1, (paths1 & ~(7u << (4 * fWin))) | (fScanCh << (4 * fWin)));
	__asm__ volatile ("dsb sy" ::: "memory");
	dpuWrite(fDecon, DECON_SHADOW_UPDATE, dpuRead(fDecon, DECON_SHADOW_UPDATE) | mask);
	for (unsigned i = 0; i < 400 && (dpuRead(fDecon, DECON_SHADOW_UPDATE) & mask) != 0; i++) {
		delay(100);
	}
	fScalerOn = false;
}

/*
 * The panel held under `nits` (0: as asked again) while the chip is hot, from the thermal loop
 * (bsd/dev/redwood_exynos_acpm.c): what CoreBrightness asks is kept, and comes back when the cap goes.
 */
extern "C" void redwood_fb_thermal_cap(uint32_t nits);
void
redwood_fb_thermal_cap(uint32_t nits)
{
	uint32_t cap = nits << 16;

	if (cap == gBLThermalCap) {
		return;
	}
	gBLThermalCap = cap;
	if (gFramebuffer != NULL && gFramebuffer->fBLCall != NULL && gFramebuffer->fDsim != NULL) {
		thread_call_enter(gFramebuffer->fBLCall);
	}
}

extern "C" void redwood_fb_quiesce(void);
extern "C" void
redwood_fb_quiesce(void)
{
	if (gFramebuffer != NULL) {
		gFramebuffer->loaderQuiesce();
	}
}

extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishBacklight(fb);
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
