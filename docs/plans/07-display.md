# 07 — Display: no tearing, brightness, more frames, 120 Hz

Goal: a picture that never tears, a brightness slider that changes the panel, a higher and
steadier frame rate, and in the end the panel's 120 Hz mode.

## How the display works now

- The panel is a Samsung **s6e3hab "hubble3"**: 1440×3200, DSC 3:1, a MIPI **command‑mode** panel with
  its own frame memory.
  - DECON_F (`0x19050000`) sends it one frame over DSI on every TE pulse. Live trigger: `HW_SW_TRIG_CONTROL` = `0x1001`,
    i.e. HW trigger on and not masked, so the panel is refreshed from RAM about 60 times a second.
  - DECON reads the frame through window 5, channel 0, from S‑Boot's buffer at `0xf1000000`.
- `MiniIOMobileFramebuffer` scales CoreAnimation's 1125×2436 frame with the CPU (6 threads, bilinear)
  **straight into that buffer** while DECON may be reading it. That is the tearing.
- The CPU scale/copy (about 11–20 ms per frame) is also what limits the frame rate.
- **The DMA (IDMA, `0x1909x000`) and DPP (`0x1904x000`) register blocks read as 0 on our side.** The cause is
  the DPU's TrustZone controller, not a clock (D7). DECON and DSIM registers work fine.
  (D1 probed `0x1904x000` thinking it was the IDMA; it is the DPP.)
  - So switching the DMA base address (double buffering, D2 below) is blocked until these blocks
    are ungated (D7).
- Kernel `IOLog` output only reaches the loader log if the kernel returns normally. Display
  statistics therefore go to `loader_log_line` (look for `fb:` lines).

Register sources (session `files/dpu-ref/`, from Samsung's exynos990 kernel):
- `regs-decon.h`, `decon_reg.c`, `decon_core.c`
- `regs-dsim.h`, `dsim_reg.c`, `dsim_drv.c`
- the panel driver `s6e3hab*.h`
- z3s `docs_DISPLAY.md`

| Block | Phys | Registers used |
|---|---|---|
| DECON_F | `0x19050000` | `GLOBAL_CONTROL 0x0` (bit 5 IDLE = no frame being sent, bit 4 RUN, bit 8 command mode); `SHADOW_REG_UPDATE_REQ 0x60`; `SECURE_CONTROL 0x64`; `HW_SW_TRIG_CONTROL 0x70` (bit 0 HW_TRIG_EN, **bit 4 HW_TRIG_MASK_DECON**); `HW_TE_CNT 0x78`; `FRAME_COUNT 0x2A0`; `OUTFIFO_COUNTER0 0x474` (bits 16–29 = output line) |
| DSIM0 | `0x190C0000` | `PKTHDR 0x58` (id, data0, data1), `PAYLOAD 0x5C` (4 bytes LE per write), `FIFOCTRL 0x68` (bit 10 header FIFO empty, bit 11 full, bit 9 payload full), `LINK_STATUS1 0x0C`, `CMD_CONFIG 0x80` |
| DPU_DMA common | `0x19090000` | version `0x0`, QCH `0xC`, CGEN `0x40` (D‑TZPC: reads 0 until restored) |
| IDMA ch c | `0x19090000 + 0x1000·(c+1)` | `ENABLE 0x0`, `SRC_SIZE 0x10`, `BASE_Y8 0x40`, in‑use copy `+0x800` (D‑TZPC) |
| DPP ch c | `0x19040000 + 0x1000·(c+1)` | colour pipe / scaler (D‑TZPC) |

## Steps

| ID | Step | State |
|---|---|---|
| D1 | `fb_dpu=1`: read and log the live state of DECON and DMA | done: DECON readable, IDMA reads 0 |
| D2 | `fb_flip=1`: two buffers, switch the IDMA base | **blocked** by gated IDMA (needs D7) |
| D2b | **`fb_te=1`: tear‑free without the DMA.** Draw into a cached back buffer. Then mask DECON's HW trigger, wait for `IDLE` (no frame being sent), copy only the changed rows into the panel buffer (helper threads), and unmask. The panel keeps showing its own memory meanwhile; the next TE sends the complete new frame. Measured at start‑up (`fb: te:` lines): TE rate, transfer duration, idle bit behaviour. Per 5 s it reports idle wait, copy time and races (`FRAME_COUNT` changing during a copy). | **written, first run pending** |
| D6 | ✅ **Brightness.** CoreBrightness (backboardd) calls `IORegistryEntrySetCFProperty(IOMFB service, "IOMFBBrightnessLevel", SInt32 nits·65536)` (from disassembling CoreBrightness `CBDisplayModuleiOS`; it is skipped only if the service has `ignore-IOMFB`). Our `IOMobileFramebuffer` gets `setProperties()`, maps nits → panel DBV with Samsung's hubble3 table (`lum_tbl` 2…420 nits ↔ `dbv_table` 0x00A…0x7FF), and sends `F0 5A 5A` · `6A hi lo 01` · `F7 0F` · `F0 A5 A5` through the DSIM command FIFO (DCS long write `0x39`, short `0x15`). Coalesced on a thread call. Boot‑arg `fb_bl=1`. | **written, first run pending** |
| D3 | Real vsync: CoreAnimation's clock and our vsync notifications follow DECON's TE counter instead of the driver's timer | next after D2b |
| D7 | **Ungate DPU DMA/DPP.** Research (`files/dpu-ref/DMA_GATING_REPORT.md`): (1) our probe read the wrong block. Per `dpp_drv.c`, **IDMA is `0x19091000 + 0x1000·c`** (DMA common `0x19090000`); `0x19041000…` is the DPP colour pipe. (2) Both read 0 because the DPU's **D‑TZPC** was never restored after S‑Boot. Linux does it when it powers `pd-dpu` on: `smc(0x82000410, 1, 0x19010204, 2)`, the same call that unlocked the GPU (`redwood_mali.c`). `fb_dpu=2` makes that call and logs the DMA common version (expect `0x05000000`), IDMA0 base (expect `0xf1000000`) and DPP0, before and after. Fallbacks: `smc(0x82002010, 0, 3, 0)` (channel protection off); CMU_DPU `0x19000000` gates. | **written, run pending** |
| D4 | No CPU copy at all: a DPP with a scaler reads CA's surface directly | **written, `fb_scaler=1`** (25 Oct): see "D4 as built" below |
| D5 | **120 Hz.** The panel only does 120 Hz (HS) at **1080×2400** (Samsung VRR table), so this is a full mode set. Panel: `9D 01`, PPS `9E`+128, CASET/PASET, scaler `BA 02 26 08 08 F3`, OSC/FPS `60 20`/SRC_AMP + `vfp_hs`/`ltps` timing. DECON: `BLENDER_BG 0x200`, OUTFIFO `0x120–0x128`, `SCALED_SIZE 0xA0`, DSC0/1 `CONTROL3` + PPS, window 5 end `0x10FC`. DSIM: `RESOL 0x3C`. **Plus the IDMA source size/stride, which needs D7**: z3s skipped it and got partial frames. Also cuts CPU work about 45 % (2.6 M instead of 4.6 M pixels per frame). | after D7 |
| D8 | Screen off/on: `RequestPowerChange` (selector 12) → DCS `28`/`10` and `11`/`29`, DECON trigger masked while off | after D6 |

## Order

D2b and D6 go in the same build (one phone run). Then D3. D7 is the research gate for D5 (120 Hz)
and D4 (zero copy).

## What to look for in a run

- `fb: te: ...` at start: whether `IDLE` drops during transfers, the TE rate, the transfer length.
- `fb: te ...` every 5 s: frames, average and maximum idle wait, copy µs, races, timeouts.
- `fb: bl ...`: every brightness request (nits → DBV) and whether the DSIM FIFO drained.
- On the phone: no tearing when scrolling. The Control Centre slider changes the panel's brightness.

## Results log

- 2026‑10‑25: D1 showed every IDMA register reads 0. D2 (flip) is parked; D2b and D6 written.
- Baseline (GL, before D2b; previous run): 15–30 frames a second while moving, 10–20 ms a frame in
  the driver, half of the frame intervals under 34 ms.
- Run 1 (`fb_te=1 fb_bl=1`): **tearing mostly gone.** Masking the trigger holds frames back (0 sent in
  40 ms masked). DECON sends at about 58–60 Hz, 8.2 ms per frame. Copies took 1–2.5 ms, waits 2 ms on average
  (up to 8 ms). The driver spends 12–25 ms per frame, the same as before. Register `0x78` is not a TE counter
  (about 4 M/s). **Brightness did nothing:** CoreBrightness never called `setProperties`. Disassembly
  (`CBDisplayContaineriOS`) shows it only builds its display module for a service matching
  `IOPropertyMatch {backlight-control: true}` **and** conforming to `AppleARMBacklight`.
- Run 2 (`fb_te=1 fb_bl=1 fb_dpu=2`): an `AppleARMBacklight` service (child of disp0, `backlight-control`,
  `IODisplayParameters.brightness` 0…65536) whose writes and `IOMFBBrightnessLevel` both go to the panel. The
  race check now uses INTERRUPT_PENDING frame‑start (cleared after idle, re‑checked 30 µs later and after the copy).
  Per‑frame time is split into compare and scale. D7 probe: D‑TZPC restore SMC.
- Run 2 results:
  - **D7 solved:** IDMA0 at `0x19091000` reads fine even *before* the SMC: version `0x05000000`, enable
    `0x40000000`, size `0xc8005a0` (1440×3200), base `0xf1000000` (in use the same). D1 had just probed the
    DPP. The SMC returns 0 and changes nothing, so D2 (flip) and the D5 IDMA resize are unblocked. Note:
    `0x19100000` is the DPU **SysMMU** (z3s's faulting "DPU_DMA"); never read it raw.
  - **Tearing:** very little left. 0 raced and 0 late frames across all copies.
  - **Brightness:** CoreBrightness now drives the backlight service (`brightness`/`backlight-level`
    0…65536, plus `brightness-nits`, `DisplayBrightness`, `IOMFBBrightnessLevel` = 0). All DCS writes
    report "sent" (the FIFO drains, sometimes after waiting up to 5 ms for a frame), **but the panel does
    not change.** It powers up at maximum brightness. y2s is **hubble2** (dtbo panel‑name
    `s6e3hab_hubble2_a3_s0_*`), with the same 6A/F0/F7 bytes as hubble3.
  - **Power button:** iOS sends selector 12 (0 off / 1 on) and brightness 0. The image goes black but the
    panel stays lit.
  - **Speed:** about 6 ms per frame goes to comparing (single‑threaded), 1–12 ms to scaling.
- Run 3 (`fb_te=1 fb_bl=1 fb_dpu=1`): DCS reads of 0A/54/6A/52/04 at start and after writes 1–2;
  `53 00` (HBM off) added to the sequence; selector 12 → DCS 28/29; compare shared across threads.
- Run 3 results:
  - **Screen off/on works** (DCS 28/29, short packets): D8 done.
  - DCS reads work: ID `81 11 04`, power mode `9F`, `54` = `00` (HBM off, so not the cause).
    `6A` and `52` read `00 00 00` even after writes.
  - Brightness still unchanged. hubble2's brightness is **gamma (CA, 34 bytes) + AOR (B1)**. Samsung's
    tables for both come from the panel's own flash (`DIM_FLASH`, `s6e3hab.c` `init_aor_table`); `6A` alone
    does nothing visible.
  - Compare 6 ms → **2.3 ms**. Scaling is now the cost: 2–19 ms depending on the rows changed.
- Run 4 (`fb_te=1 fb_bl=1`): **brightness by AOR** used as a dimmer, from the AOR found at boot (full) to
  `0x0C77` (darkest), linear in nits. After each write, B1 is read back; if it does not match, the write is
  retried with PKT_GO (Samsung's send mode). The gamma (CA) is logged.
- Run 4 results:
  - **The slider works (AOR dimming).**
  - Full brightness was a white screen: B1 read `00 00` (Samsung's registers read 0 while locked), so the
    "full" AOR was 0, which the panel does not take.
  - A spike on the first slider move: the PKT_GO retry (writes 1–3) most likely sent a corrupt packet.
  - A red tint at the darkest setting (`0x0C77`: 9 of 3200 lines lit).
- Run 5: full = `0x01E4` (Samsung's AOR from 265 nits up), darkest `0x0C27`, PKT_GO retry removed, reads done
  with the level‑2 key on (to read the real B1/CA).
- Run 5 results: **brightness works and looks right** across the range (user). With the key on, the reads
  are real:
  - as found: AOR `05 0c`, DBV `6a = 00 01 00`, gamma `ca = 2a 00 00 00 80…80 99 90 00` (neutral `0x80` offsets)
  - after the writes: AOR `0c 27` → `07 95`, DBV `03 8c 01`

  So the writes land, and the panel applies its own calibration on top of our AOR. **D6 done.**

### Next big step: D4 zero‑copy through a DPP scaler

Now that IDMA/DPP are reachable: keep CoreAnimation's full‑screen BGRA surfaces **physically contiguous**
(MiniIOSurface pool; the DPU has no SysMMU translation here: IDMA reads `0xf1000000` physical). Then point
a scaler channel (VGRFS = channel 5) at the surface each swap: 1125×2436 → 1440×3200 in hardware, with the
base latched by the shadow update (no tearing). Drops compare + scale + copy (about 5–25 ms per frame) to
about 0. Needs: DPP scaler/CSC programming (`dpp_reg.c`), window ↔ channel mapping (`DATA_PATH_CONTROL_1`),
and the swap's layer rectangles mapped to windows.

**D4 programming spec (research of `research/dpu-ref`, 25 October; file:line refs are in those sources):**
- **Scaler channels:** only **3 (VGS)** and **5 (VGRFS)** have one (`exynos9830-dpu.dtsi` attr bit 5 `SCALE`);
  channel 0, the one S‑Boot uses, cannot scale. Use **5**: IDMA `0x19096000`, DPP `0x19046000`, AXI port 0
  like GF0, scale up ≤ 8×, down ≤ 2×, source ≤ 4096×4096, 1‑pixel alignment, minimum 16×16.
- **Order** (Samsung `dpp_reg_configure_params`, `dpp_reg.c:1074-1104`): CSC, then scale ratios + coefficients,
  then sizes, rotation/flip, base, compression, format.
- **IDMA 5:** `IN_CON 0x08` (format BGRA8888 = `3 << 8`, AFBC/SBWC/BLOCK off, no rotation), `SRC_OFFSET 0x14 = 0,0`,
  `SRC_SIZE 0x10` and `IMG_SIZE 0x18 = 1125×2436`, `BASE_Y8 0x40` = physical address,
  `SRC_STRIDE_0 0x50` = the surface's bytes per row (check its encoding by reading it back).
- **DPP 5:** `IMG_SIZE 0x18 = 1125×2436`, `SCALED_IMG_SIZE 0x2c = 1440×3200`, `MAIN_H_RATIO 0x44 = (1125 << 20) / 1440`,
  `MAIN_V_RATIO 0x48 = (2436 << 20) / 3200` (20‑bit fraction); **coefficient tables are required**: 8‑tap
  horizontal `h_coef_8t`, 4‑tap vertical `v_coef_4t`, bank 0 for upscaling (`dpp_reg_set_h/v_coef`,
  `dpp_reg.c:398-448`). CSC bypass for RGB.
- **DECON:** `DATA_PATH_CONTROL_1 0x218`: window 5's field `(ch & 7) << (4·win)` from 0 to 5; window 5 rectangle
  `WIN_START_POSITION(5) = 0x1008 + 5·0x30 = 0`, `WIN_END_POSITION(5) = 0x100c + 5·0x30 = (3199 << 16) | 1439`.
  DSC/DSIM unchanged: DECON still outputs 1440×3200.
- **Per frame:** GPU done, `dsb sy`, write `BASE_Y8`, `SHADOW_REG_UPDATE_REQ 0x60 |= GLOBAL | (1 << 5)`, wait for the
  request to clear; the base latches at the next TE frame (in‑use copy at `+0x800`). Never reuse a surface while it
  is the in‑use base.
- **The real prerequisite: physically contiguous ~11 MB frames** (1125×2436×4) for CoreAnimation's display
  surfaces, 2–3 of them, since the DMA reads physical memory and the DPU SysMMU (`0x19100000`) must not be touched.
  A loader‑reserved pool is the likely route; note that mapping the 32 MB Wi‑Fi pool with `kmem_alloc` panicked
  (`KERN_NO_SPACE`, xnulog‑wifi‑run.txt), so the pool needs another mapping method.
- **Fallback:** if the shadow request does not clear, the in‑use base does not change, or a DMA/DPP error bit is
  set: stop channel 5, map window 5 back to channel 0, return to the CPU path for the rest of the boot.

### D4 as built (`fb_scaler=1`, 25 October)

- **Surfaces:** `MiniIOSurface.cpp` makes every full-screen 32-bit surface (at least 2 M pixels, not wrapped,
  one plane) from one piece of physical memory below 4 GB (`inTaskWithPhysicalMask`, `kIOMemoryPhysicallyContiguous`,
  mask `0xFFFFF000`). The kept-memory pool keeps contiguous and paged pieces apart. `MiniIOSurface::phys` is the
  pixels' physical address (0: not contiguous). If no such piece is free, the surface is made as before and shown by copying.
- **Channel 5** is set up once at start-up, while it feeds no window. Copied from S-Boot's channel: IDMA `IN_CON`
  (format, the copy path's byte order), strides, SBWC/recovery/deadlock/bus/cache/QoS/gating, and the DPP's enable
  bits, `IN_CON`, CSC and gating. Set for scaling: sizes, ratios `(src << 20) / dst`, and a **Keys bicubic of our own**
  (a = −0.5, 9 phases, sum 512, the same in both banks; horizontal taps 2..5 of 8, vertical 0..3 of 4). Then
  `SFR_UPDATE_FORCE`. Before that, a write/read-back test checks that the channel's registers are live at all.
- **Per frame:** a swap with one layer (layer 0) that is full screen, BGRA, contiguous, with an untrimmed source
  gets IDMA5 `BASE_Y8` and stride. The **window S-Boot set up stays as it is, only its channel changes**
  (`DATA_PATH_CONTROL_1` field of window 0: S-Boot's channel → 5), and the shadow update latches both with the next
  frame. No window blending or geometry is touched.
- **Surface lifetime:** the surface on the panel and the one asked for are held, and their IOSurface use count is
  raised (`MiniIOSurfaceDisplayUse`, so `IOSurfaceIsInUse` is true for CoreAnimation), until the next frame is taken.
- **Fallback:** a frame that does not fit is drawn by the CPU (in full, since the panel buffer is frames behind),
  and the window goes back to S-Boot's channel. Any IDMA5 error bit (AXI, configuration, deadlock, …), a DPP
  configuration error, or three switches not taken: back to the CPU path for the rest of the boot.
- **What the log says:** `fb_scaler: channel 5 ready …` and `… now: …` (the registers), `fb_scaler: on`, the first
  frames' `taken after N us` with the base in use, and every 5 s `N frames read by the controller itself, M drawn
  by the processor`.
- **Run 12:** the scaler refused at start-up: it looked for S-Boot's row length in `SRC_STRIDE_0/1`, and both read 0
  (the phone ran on the CPU path as before). Samsung's `idma_reg_set_coordinates` gives the encoding: the row length is
  `SRC_SIZE`'s width in **pixels** (`f_w`; S-Boot's is `0xc8005a0`, 1440 × 3200 for 5760 bytes a row), and `IMG_SIZE` at
  `SRC_OFFSET` is the visible part. The stride registers count only with `IDMA_PLANE_n_STRIDE_SEL`. Fixed: each frame
  writes `SRC_SIZE = height << 16 | bytesPerRow / 4` (CA's 4544-byte rows give 1136) and `IMG_SIZE = 1125 × 2436`.
- **Runs 13–14:** channel 5 set up cleanly (registers live, no configuration errors), but no frame qualified:
  CoreAnimation's surfaces were never in one piece. `kIOMemoryPhysicallyContiguous` is honoured only without a
  system mapper, and even `kIOMemoryHostPhysicallyContiguous` fails: XNU's VM manages no memory below 4 GB at all
  (`vm_page_find_contiguous(num=687, low=262143): … scanned 1 pages`). The loader gives XNU a window above 4 GB.
- **Display pool (prepared):** the loader reserves 64 MB below 4 GB (`reserve_display_pool`, like the Wi-Fi DMA
  pool) and publishes `/chosen display-pool-base/-size`. MiniIOSurface puts full-screen 32-bit surfaces in 14 MB
  slots of it (`IOMemoryDescriptor::withPhysicalAddress`). `MiniIOSurface::memory` is now an `IOMemoryDescriptor`;
  pool surfaces hold `poolMap`/`poolSlot` and give the slot back on free. Memory outside XNU's is device memory
  unless mapped otherwise, so **every mapping of a pool surface is write-combined**: the kernel's, each client's
  (`addRef`), and RedwoodMali's `clientMemoryForType`.

### Run 17 (25 October): zero copy at 60 fps, but tearing -- the swap did not wait

With the latch wait in the presenter: up to 301 frames per 5 s read by the controller itself (60 fps), ~8-9 ms a
frame waiting for the switch. User: icons/dock glitch, the search pull-down, Settings tearing while scrolling,
Control Centre elements flashing. Cause: `SwapEnd` only queues the swap (present()) and returns; the presenter
latches it later. CoreAnimation (two surfaces in turn) draws its next frame into the surface the panel still
shows. Fix: tickets -- present() waits (≤ 100 ms) until the presenter has retired its swap, i.e. the frame is on
the panel, when the scaler is on (`fb_swapwait=0` turns it off); and a queued swap's surface is counted in use
from present() on (as Apple's IOMFB does), so a CoreAnimation that looks finds it busy and may take a third.

### Run 18: no tearing; 33-35 fps; black band on blurred materials

Swap wait works (no tearing). CoreAnimation still turns over two surfaces only (in-use from queueing did not
make it take a third). Swipes at 33-35 fps with the GPU 85 % idle, commit 1.6 ms, GPU 3.6 ms, and 6-7.5 ms a
frame waiting for the switch: the vsync was a free-running timer, out of phase with the panel, so the window
server -- blocked in the swap until the frame is taken -- missed every other vsync. Run 19: `armVSync()` puts
the vsync 0.3 ms after the scaler's last latch (`fLatchTime`), period by period.
Black band at the top of the search page / App Library (blurred materials), seen before in runs 14-15 whenever
a UBO was uploaded shorter than 64 KB: the NIR extent is now never less than Apple's `air.buffer_size`.
