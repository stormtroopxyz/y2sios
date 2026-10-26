# PROGRESS2510: HackiOS on the Galaxy S20+ (y2s), work of 24–25 October 2026

This is the handover for the work done on 24–25 October 2026: Metal, the display, Wi‑Fi, Bluetooth and
audio planning. It assumes you have read [handoff.md](handoff.md), which covers the boot chain, recovery key
combos, the touch saga and the original GPU notes. Working plans, with per‑step results logs, are in
[PLANS/](../plans/README.md). Reference sources and research reports are now in [research/](../research/).

---

## 0. State at a glance

| Area | State | Where |
|---|---|---|
| Boot, SMP (6 cores), touch, buttons, battery | working (before this session) | handoff.md, PLANS/00–03 |
| GPU: OpenGL ES on the Mali‑G77 (CoreAnimation draws on the GPU) | **working, the everyday image** | PLANS/04-gpu.md |
| Metal on the Mali | SpringBoard runs, with drawing errors; **parked** | PLANS/05-metal.md |
| Display tearing | **mostly fixed** (`fb_te=1`): copy under a masked TE trigger | PLANS/07-display.md D2b |
| Display brightness | **working** (`fb_bl=1`): slider, auto, full range; AOR dimming | PLANS/07-display.md D6 |
| Screen off/on (power button) | **working** (`fb_bl=1`): DCS 28/29 | PLANS/07-display.md D8 |
| Display DMA registers (IDMA) | **readable**: the address was wrong, no unlock needed | PLANS/07-display.md D7 |
| Frame rate | unchanged (about 15–40 fps). Compare cost cut from 6 to 2.3 ms; the CPU scale is now the bottleneck | PLANS/07-display.md |
| 120 Hz | planned (needs an FHD mode set; now unblocked) | PLANS/07-display.md D5 |
| Wi‑Fi | PCIe driver + DMA pool written; **first hardware run flashed, not yet read**; Linux brcmfmac compiles but is not yet linked | PLANS/06-wireless.md |
| Bluetooth | iOS attachment model **solved by disassembly**, design written; no kernel code yet | PLANS/06-wireless.md "Bluetooth" |
| Sound | hardware research done (ABOX, CS35L41); no code yet | PLANS/08-audio.md |
| Firmware blobs (Wi‑Fi, BT, audio DSP, amps) | **extracted** from the stock AP tar | fw-stock/INDEX.md |

---

## 1. The phone right now, and the first thing to do

The phone has the **Wi‑Fi test image** flashed (md5 `38c310ca2c968abfed40229ffec1c12d`). Its kernel command line:

```
debug=0x8 rd=md0 touch_ts=1 bms=1 mali=1 cpumask=0x3f fb_threads=6 fb_te=1 fb_bl=1 wifi_pcie=3
```

This is the everyday GL image plus the two display features plus the first Wi‑Fi bring‑up. About 20 s after
boot, `redwood_exynos_pcie.c` does the following:
1. Powers the BCM4375 (WLAN_REG_ON).
2. Brings up the PCIe link.
3. Reads the chip's PCI id.
4. Grants the 32 MB DMA pool through Samsung's hypervisor.

**It has not been run and read back yet.**

**First action:**
1. Boot the phone and let it sit about 40 s on the home screen.
2. Hold Vol‑Down + Power for 3 s. This ends the run and saves the log.
3. Enter Download Mode.
4. Do the log routine in §3.
5. Then: `grep -a '^wifi:' xnulog.txt`

What each line means is in §5.2 and in [PLANS/06-wireless.md](../plans/06-wireless.md) ("W2/W3 implementation").

If the phone froze during that run, the log may be missing. Rerun with `wifi_pcie=1` (read‑only) first,
then `wifi_pcie=2` (link only, no HVC).

---

## 2. Orientation

```
Exynos-iOS/                         (git root; almost everything is still untracked: commit soon)
├── handoff.md                      the original handover (boot chain, recovery, touch, GPU)
├── PROGRESS2510.md                 this file
├── PLANS/                          working plans 00–08 + README status board (each has a results log)
├── research/                       reference sources and reports copied out of the agent session (see §8)
├── fw-stock/                       firmware/configs pulled from the stock AP tar (INDEX.md, vendor.img)
├── xnulog.txt / xnulog.bin         the last kernel log read off the phone
├── samloader-rs/fw/                stock firmware tars (AP 7.8 GB, the super image, misc.bin)
├── heimdall-arm64/                 Download‑mode flasher (used only to restore `misc`)
└── hackios-xnu-kernel-main/
    ├── y2s.sh                      build / payload / pack / flash / log (THE script)
    ├── xnu-src/xnu-8796.121.2/     the kernel tree (exported to xnu/ by `y2s.sh kernel`)
    │   ├── iokit/Kernel/MiniIOMobileFramebuffer.cpp   display driver (tear fix, brightness, DSIM)
    │   ├── iokit/Kernel/MiniIOSurface.cpp             IOSurfaces
    │   ├── iokit/Kernel/RedwoodMali.cpp, bsd/dev/redwood_mali.c   GPU
    │   ├── bsd/dev/redwood_exynos_pcie.c              NEW: Wi‑Fi PCIe link + DMA grants
    │   ├── bsd/dev/redwood_input.c                    where every redwood driver is started
    │   └── bsd/conf/files                             where driver files are registered
    ├── xnuloader/loader.c          the UEFI app that loads XNU (now also reserves the Wi‑Fi DMA pool)
    ├── userland/                   iOS‑side code, build-gpu-y2s.sh, mkjobs.py, rc.jobs.*
    ├── tools/mk-mesa.sh            Mesa (Panfrost + our AIR→NIR Metal translator)
    ├── tools/mk-ath11k.sh          upstream's Linux‑driver‑in‑XNU recipe (Qualcomm)
    ├── tools/mk-brcmfmac.sh        NEW: the same for Broadcom brcmfmac (in progress)
    ├── wifi/lx/                    LinuxKPI glue (shared with ath11k; brcmfmac/ subfolders are new)
    └── ios165/                     the iOS 16.5 (20F66, iPhone10,3) image and its dyld shared cache
```

---

## 3. The build → flash → log loop (exact commands)

All from `hackios-xnu-kernel-main/` unless noted.

```sh
# build the kernel (refuses on any compiler warning in our files)
./y2s.sh kernel                       # -> out/mach.y2s; checks it is a GICv2 kernel

# loader + command line, then the recovery image (UEFI @0, kernel @32 MiB, root.apfs @48 MiB)
./y2s.sh payload "touch_ts=1 bms=1 mali=1 cpumask=0x3f fb_threads=6 fb_te=1 fb_bl=1"
./y2s.sh pack

# write it (phone in Android, adb root works), verify md5, `adb reboot recovery` boots it
./y2s.sh flash
```

**Ending a run and reading the log:**
1. Hold Vol‑Down + Power for 3 s. The kernel returns to the loader, which writes the log to `recovery` + 16 MiB.
2. Enter Download Mode (handoff.md §12).
3. From the repo root, restore `misc` so the next boot is Android:
   ```sh
   timeout 90 heimdall-arm64/heimdall-frontend.app/Contents/MacOS/heimdall flash --MISC samloader-rs/img/misc.bin --stdout-errors
   ```
4. Wait for `adb devices` to show the phone.
5. From `hackios-xnu-kernel-main`, read the log: `timeout 120 ./y2s.sh log`. It reads 8 MiB, strips NULs and writes
   `../xnulog.txt`.

**Userland image** (only when userland changes): `cd userland && RC_JOBS=rc.jobs.y2s ./build-gpu-y2s.sh`. This is the
everyday GL image. The Metal variant is in PLANS/05-metal.md.

**Rules learned the hard way:**
- `GIC_V2=1` must be exported for both the kernel and the loader. `y2s.sh` does it.
- The log can contain NUL bytes. `y2s.sh log` strips them; it no longer cuts there.
- `IOLog` output reaches the loader log only via the msgbuf, appended when the kernel returns cleanly. Anything
  that must survive goes through `loader_log_line()`. The display code uses `fbSay()`; the Wi‑Fi code uses
  `wlog()` with a `wifi:` prefix.
- **Shared build lock.** While agents were working in parallel, every build/pack took a lock:
  ```sh
  until mkdir /tmp/y2s-build.lock 2>/dev/null; do sleep 15; done; ...; rmdir /tmp/y2s-build.lock
  ```
  Keep the habit if you run parallel workers.
- **Kernel size:** `out/mach.y2s` is **16,054,616 bytes**, and its slot is 16 MiB (16,777,216). That leaves
  about **700 KB**. brcmfmac alone is about 270 KB (text + data). Before linking it, move `root.apfs` from
  48 to 56 MiB: `--roff 3800000` in `y2s.sh payload` and `pack` (PLANS/README.md "Watch").

---

## 4. What was done (chronological summary)

### 4.1 Metal (first half of the session): parked

Full detail is in [PLANS/05-metal.md](../plans/05-metal.md) and checkpoint notes. In short:
- Our Metal driver (`userland/src/redwoodmetal.m` + `tools/mesa/air/air_metal.c`, AIR→NIR→Panfrost) runs
  SpringBoard after many fixes:
  - waits: the deadline clock skew in `drmSyncobjWait` (`tools/mesa/redwood-drm/redwood_drm.c`)
  - the viewport/IO lowering order
  - integer render targets
  - UBO slot 0
  - scissor, the blit encoder, multisample resolve, depth only when used, object retention
- Remaining wrongness (square corners, no folder/dock blur, Settings crash): CoreAnimation's uber‑shader
  `fixed_frag_lph_cph` is specialised by **58 function constants**, and we pass them all as 0. Our hooks
  never saw CoreAnimation set them (`0 values (tied)`).
- Next step there: read `MTLFunctionConstantValuesInternal`'s ivars, or call
  `serializedConstantDataForFunction:...` (see 05-metal.md).
- **Switched back to OpenGL ES.** Afterwards the GL image showed sheared icons and backboardd crashes. Cause:
  a kernel change that rounded `IOSurfaceBytesPerRow` up to 64. That breaks surfaces that wrap client memory.
  **Reverted.** Never change a requested bpr (`MiniIOSurface.cpp`).

### 4.2 Display (`iokit/Kernel/MiniIOMobileFramebuffer.cpp`)

**How the display works.**
- The panel is a Samsung s6e3hab **"hubble2"**. The device‑tree overlay names `s6e3hab_hubble2_a3_s0_*`;
  z3s's notes are for the S20 Ultra's hubble3.
- 1440×3200, DSC 3:1, MIPI **command mode**, TE‑triggered.
- DECON_F (`0x19050000`) sends a frame on every TE through window 5 / DMA channel 0, from S‑Boot's buffer at
  `0xf1000000`. Each send takes **8.2 ms**, at **58–60 Hz**.
- iOS believes it is an iPhone X (1125×2436). The kernel **scales every frame with the CPU** (6 threads,
  bilinear) into the panel buffer.

**D2b: tear fix, `fb_te=1`.** Frames are drawn into a cached back buffer (`fBack`). Then `tePresent()`:
1. Sets **HW_TRIG_MASK_DECON** (bit 4 of `0x70`). The panel keeps showing its own memory.
2. Waits for DECON **IDLE** (bit 5 of `GLOBAL_CONTROL`).
3. Clears FRAME_START pending (bit 12 of `INTERRUPT_PENDING 0x4C`, write 1 to clear), waits 30 µs, and if it is
   set again (a TE that slipped in just before the mask), waits again.
4. Copies **only the changed rows** to the panel buffer, on the helper threads (`RowJob.copy`).
5. Checks for a race and unmasks.

`teMeasure()` at start‑up proved masking holds frames back: 0 frames sent in 40 ms masked.

Results:
- "Very little tearing" remains; there were 0 raced and 0 late frames in all runs.
- Each frame costs a copy of 0.5–2.8 ms plus a wait of about 2 ms on average (8 ms at most).
- Remaining visible artefacts are probably uneven frame pacing, not tears.
- Gotcha: register `0x78` is **not** a TE counter (it counts about 4 M/s). `FRAME_COUNT 0x2A0` is the real one.

**D6: brightness, `fb_bl=1`.** This took four runs. The facts:
1. **How iOS asks.** By disassembling CoreBrightness (backboardd), extracted with DyldExtractor from the dyld
   cache: `CBDisplayContaineriOS` only creates its display module for a service that matches
   `IOPropertyMatch {backlight-control: true}` **and** conforms to the class **`AppleARMBacklight`**.
   - So the kernel now publishes `AppleARMBacklight`: a child of `disp0` with `backlight-control` = true and
     `IODisplayParameters.brightness` {min 0, max 65536, value}.
   - CoreBrightness then sets `brightness`/`backlight-level` (0…65536) on it, and `IOMFBBrightnessLevel`
     (nits 16.16) on the framebuffer. Both paths feed `brightnessRequest()`, then a thread call, then
     `blWork()`.
2. **Talking to the panel.** We write DCS commands through DSIM0 (`0x190C0000`):
   - payload words go to `0x5C`, then the header to `0x58`
   - long write `0x39`, short writes `0x05`/`0x15`
   - wait on `FIFOCTRL 0x68`
   - reads: max‑return‑size `0x37`, then a DCS read `0x06` with BTA (bit 24), answer in `RXFIFO 0x60`
   - writes and reads are serialised by `fDsimLock`
3. **What the panel needs.** Its "DBV" register `6A` alone does nothing visible. hubble2's brightness comes
   from gamma (`CA`) + **AOR** (`B1`, the share of each frame the OLEDs are held off). Samsung's per‑level
   tables live in the panel's flash.
   - What works: **AOR as a dimmer**, linear in nits:
     `AOR = 0x0C27 − (0x0C27 − 0x01E4)·nits/420`
   - The sequence:
     ```
     F0 5A 5A · B1 hi lo · 6A hi lo 01 · 53 00 · F7 0F · F0 A5 A5
     ```
     (level‑2 key, AOR, DBV, HBM off, update, key off)
   - Endpoints. AOR 0 gives a white screen. `0x0C77` (Samsung's darkest) leaves 9 of 3200 lines lit and
     looks red. So the brightest is `0x01E4` and the darkest is `0x0C27`.
4. **Samsung's own registers read 0 unless the level‑2 key is on.** With the key on, the bootloader's state is
   AOR `05 0C`, DBV `00 01 00`, gamma `2A 00 00 00 80…80 99 90 00`. Our writes do land.
5. Verified by the user: the slider works, both ends look fine, and there is no spike.

**D8: screen off/on.** iOS calls framebuffer selector 12 (`RequestPowerChange`) with 0 or 1. The kernel now sends
DCS `28`/`29` (`displayPower()`). Verified: the screen really turns off.

**D7: display DMA registers.**
- The first probe read `0x19041000`, which is the **DPP** (colour pipe). The IDMA is at **`0x19091000 + 0x1000·c`**;
  the DMA common block is at `0x19090000`.
- At the right address everything reads fine, **without** any secure call: version `0x05000000`, size
  `0xc8005a0`, base `0xf1000000`.
- The D‑TZPC SMC (`fb_dpu=2`) returns 0 and is not needed.
- **Never read `0x19100000`.** That is the DPU **SysMMU**; z3s saw it fault (they called it "DPU_DMA").

**Speed.** Per‑frame time in the driver is 5–28 ms. The damage compare was single‑threaded at about 6 ms; it is
now shared with the helper threads (`RowJob.compare`) and takes about 2.3 ms. The CPU **scale is now the cost**
(2–19 ms depending on how many rows changed). The 5‑second log line `fb: N frames … (comparing X, scaling Y)`
shows the split.

**Next display steps** (in PLANS/07-display.md):
- **D4 zero‑copy.** Keep CoreAnimation's full‑screen surfaces physically contiguous and point a scaler DPP
  (VGRFS = channel 5) at them each swap. The hardware scales 1125×2436 → 1440×3200, and the base switch is
  shadow‑latched, so there is no tearing and no CPU work. This is the big frame‑rate win.
- **D5 120 Hz** = a mode set to FHD 1080×2400. The panel cannot do 120 Hz at WQHD. Every DCS byte and DECON/DSC
  value is listed; the IDMA size is now writable.
- **D3** vsync from `FRAME_COUNT`.

### 4.3 Firmware extraction (no phone needed)

`fw-stock/` holds what was pulled from `samloader-rs/fw/AP_…_meta_OS13.tar.md5`:
1. The `super.img.lz4` sparse image is stream‑parsed.
2. The LP metadata gives the `vendor` (ext4) partition, which is then read with `debugfs`.

It contains:
- Wi‑Fi: `bcmdhd_sta.bin_b1` (18.41.117), `bcmdhd_clm.blob`, 12 `nvram.txt_*`
- BT: `bcm4375B1_{murata,semco,semco_sem}.hcd`
- Audio: `calliope_*` DSP firmware, `abox_tplg.*`, `cs35l40-{bot,rcv}.wmfw/.bin`, `mixer_paths*.xml`
- camera, NFC, etc.

`fw-stock/INDEX.md` lists every file and the exact commands; `extract_super_partitions.py` is kept there. Which
NVRAM/HCD variant this phone needs (Murata or SEMCO) is still unknown: try SEMCO first (z3s used it).

### 4.4 Wi‑Fi (../plans/06-wireless.md)

**W2: `bsd/dev/redwood_exynos_pcie.c`** (822 lines; written by an agent, reviewed and extended by me). It drives
PCIe ch0, Samsung "host‑v0" (DesignWare):
- ELBI `0x133b0000`, PHY `0x133e0000`, PCS `0x133d0000`, DBI `0x133f0000`
- config window `0x11ffe000`, MEM `0x11000000`
- PMU PHY isolation `0x15860718`
- CMU_HSI1 `0x13000000` gates
- pins: REG_ON gpb0‑4, PERST# gpf0‑1, CLKREQ gpf0‑0

Boot‑args (nothing happens without them):
- `wifi_pcie=1` — read‑only look. Blocks whose clock is gated, or whose PHY is isolated, are skipped unless
  `wifi_pcie_force=1`.
- `wifi_pcie=2` — the link, `wifi_pcie_wait` s after boot (default 20, so a stall still leaves a usable phone):
  1. REG_ON cycle, then 200 ms
  2. HSI1 PCIe clocks forced on
  3. PHY isolation off
  4. PHY tables (ch0, 26 MHz ref, Gen2)
  5. DesignWare root‑port set‑up
  6. PERST# release
  7. LTSSM, up to two tries
  8. iATU CFG0
  9. the endpoint id and BAR sizes

  On failure it logs the LTSSM path and history and shuts down.
- `wifi_pcie=3` — as 2, then **W3**: the DMA pool is granted through Samsung's hypervisor,
  `hvc(0xc6000101, (1<<16)|24, granule, 0x10000, 3)` per 64 KB granule.

**W3: the DMA pool.** z3s grants only low RAM (`0x80000000–0x100000000`; their `s2mpu.c`). Our kernel's
memory is all above 4 GB (`0x880000000+`). So:
- **The loader** (`reserve_wifi_dma()` in `xnuloader/loader.c`, y2s only) takes **32 MB below 4 GB** from UEFI
  (EfiLoaderData, 64 KB aligned) and publishes it in `/chosen` as `wifi-dma-base`/`wifi-dma-size`.
- **The kernel**'s `redwood_wifi_dma_pool()` (in redwood_exynos_pcie.c) maps it normal non‑cacheable, since
  memcpy on device memory faults when unaligned. brcmfmac's coherent and streaming DMA must come from this pool.

**Expected log on success:**
- `wifi: dma pool: <phys> size 2000000`
- `ltssm (try 1): UP` and `link up: … Gen1 x1`
- `endpoint 01:00.0: id 14e4:4475`, then BAR sizes
- `W2b PASS`
- `W3 PASS: … 512 granules granted, 0 refused`

**Risk:** HVC from EL1 relies on Samsung's EL2 being there. z3s proved that on the same S‑Boot. If it is not
there, it is an undefined‑instruction panic, which shows in the log's panic text.

**W4: brcmfmac** (`tools/mk-brcmfmac.sh`, agent; interrupted mid‑way):
- Modelled on `tools/mk-ath11k.sh`: pinned Linux 7.0.14 + FreeBSD 15.1 LinuxKPI headers, licence checked per
  file (brcmfmac is ISC). The archives are downloaded into `xnu-deps/build/brcmfmac/`.
- z3s's BCM4375 changes are encoded as `wifi/lx/patches/brcmfmac/0001-bcm4375-ids-rambase-firmware.patch`,
  `0002-bcm4375-legacy-intx.patch` and `0003-bcm4375-host-cap.patch`:
  - PCI id 0x4475, chip 4375
  - RAM base `0x170000`
  - legacy INTx, not MSI
  - **host_cap `0x00821407` at shared+84**: without it the firmware never DMAs received data
- **State:**
  - The PCIe FullMAC subset compiles into `xnu-deps/build/brcmfmac/out/all.o`: __TEXT 185 KB, __DATA 83 KB.
  - `out/stubs.txt` lists 33 stubbed functions (p2p, pno, btcoex, debugfs, vendor ops). That's fine.
  - `out/undefined.txt` lists **123** symbols still to provide. Some already come from `wifi/lx/src/lx_base.c`
    and the kernel. The rest are new glue: the PCI device (config space, BARs, request_irq), netdev,
    cfg80211/wiphy registration and request_firmware.
  - The glue file `wifi/lx/src/brcmfmac/lx_brcmfmac.c` was only just started (2 lines).
  - `wifi/lx/include/brcmfmac/lx_os_pci.h` declares the kernel interfaces the glue will need.
  - Not linked into the kernel; the W4 section of the plan was not written yet.
- The kernel side it needs, all new:
  - BAR mapping (the MEM window `0x11000000`, behind iATU region 0)
  - config‑space access through the CFG0 window
  - the INTx interrupt (ELBI `0x000` pulse status, enable at `0x00c`; find the GIC SPI number in the dtb
    `pcie@133B0000` node)
  - DMA from the pool, firmware files from the root (`/fw/…`, as `redwood_adsp.c`'s `root_file()` does)
- **W5** (after the firmware boots): implement `redwood_sta` ops (see `bsd/dev/redwood_sta.c`,
  `redwood_apple80211.c`, `redwood_net.c`; upstream's ath11k path in `bsd/dev/redwood_wifi.c` is the model), so
  that wifid lists networks and joins.

z3s's working sources are in `research/wireless-ref/z3s_brcmfmac_*` (`pcie.c`, `chip.c`, `msgbuf.c`, `s2mpu.c`).
Their full story is in `z3s_WIFI_BRINGUP.md`.

### 4.5 Bluetooth (../plans/06-wireless.md "Bluetooth"): the iOS side is understood

An agent disassembled `bluetoothd` and `BlueTool` from the 20F66 image. The full findings, with addresses, are in the
plan. The essentials:
- **The gate.** bluetoothd runs only if there is a non‑zero BD address and either `/dev/cu.bluetooth`,
  `/dev/uart.bluetooth`, or an IOService named **`bluetooth`** (or `marconi-bt`). The address comes from its
  `local-mac-address`.
- **Transport.** env `BT_HCI_TRANSPORT="<transport>:<driver>:<dev>"`, or DT `transport-encoding` (1 = H4). Speed
  from `transport-speed`.
- **The "UART" is a kernel control socket, not a tty.** It uses
  `socket(PF_SYSTEM, SOCK_STREAM, SYSPROTO_CONTROL)` + `CTLIOCGINFO`/`connect` to a named control
  (BlueTool: `com.apple.uart.bluetooth`), termios via `get/setsockopt(TIOCGETA/TIOCSETA)`, then plain
  `read`/`write`.
- **BlueTool** is driven by bluetoothd over XPC (`RunBuiltinScript` boot/init/deepsleep). It holds **Apple's
  iPhone X patchram embedded** and identifies the chip by LMP subversion. On our BCM4375 it would fail, or load
  the wrong firmware, so it must **not** run.

**Design (least effort, no Apple binary patched):**
- **Kernel:**
  - an IOService `bluetooth` with `local-mac-address`, `transport-encoding`=1 and `transport-speed`
  - a kernel control `com.apple.uart.bluetooth` that bridges to the UART
  - the kernel brings the chip up itself: BT_EN, HCI reset, `.hcd` patchram via `0xfc2e`/`0xfc4c`/`0xfc4e`,
    optional baud change `0xfc18`, BD_ADDR `0xfc01`
- **Userland:**
  - `JOB_ENV="com.apple.bluetoothd:BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth"` (mkjobs.py)
  - a ~60‑line stand‑in `com.apple.BlueTool` Mach service that answers `{result:0, manufacturer:15, firmware:…}`

**Hardware:**
- UART `serial_16@10650000` (USI16)
- BT_EN gpm16‑0, dev‑wake gpg1‑1, host‑wake gpa2‑3

Samsung UART/USI/clock sources and `btbcm.c`/`hci_bcm.c` are in `research/bt-ref/samsung/`. The B1/B2 kernel
driver (`bsd/dev/redwood_exynos_bt.c`: probe, then HCI reset / version over H4) was **not written**: the agent was
interrupted. Its task description is a good spec.

### 4.6 Sound (../plans/08-audio.md): research only

- iOS side: it already exists from upstream. `userland/src/audio_plugin.c` writes 48 kHz PCM to
  `/dev/redwood-audio`, and `redwood_guard.c` provides speaker protection.
- Only the hardware back end is Qualcomm‑specific and must be replaced.
- Plan A is to **skip the DSP**: RDMA → route/SPUS/SIFS → UAIF2 (TDM master) → 2× CS35L41 on `hsi2c_31`
  (`0x40` bottom, `0x41` earpiece). From `research/abox-ref/ABOX_REPORT.md`:
  - routing, SIFS and all of UAIF2 are plain AP registers
  - the RDMA buffer/enable registers are only programmed via DSP IPC in Samsung's driver, but carry no secure
    marking; direct writes are plausible but unproven
  - `pd-aud` (`0x15861900`) has `need_smc 0x18e10204`: the same D‑TZPC restore pattern as the GPU
  - the amps have **no tone generator**
- Steps A2–A6 are defined. Nothing is coded.

---

## 5. Reference

### 5.1 Boot‑args added or used this session

| Boot‑arg | Effect |
|---|---|
| `fb_te=1` | Tear fix: back buffer + copy under a masked DECON trigger (recommended on) |
| `fb_bl=1` | Brightness and display power through DSIM: `AppleARMBacklight`, AOR dimming, DCS 28/29 (recommended on) |
| `fb_dpu=1` / `2` | Log DECON + IDMA state (`2`: also the D‑TZPC restore SMC; not needed) |
| `fb_flip=1` | Old D2 double‑buffer flip via the IDMA base. Written before the address fix (now `0x19091000`); **untested**; mutually exclusive with `fb_te` |
| `fb_threads=6` | CPU threads for scale/compare/copy |
| `wifi_pcie=1/2/3`, `wifi_pcie_force=1`, `wifi_pcie_wait=S` | Wi‑Fi PCIe look / link / link + DMA grants |
| `metal=1 camtl=1 metal_profile=1 rwm_trace=N` | Metal experiments (parked) |

**Recommendation:** add `fb_te=1 fb_bl=1` to `DEFAULT_CMDLINE` in `y2s.sh` (line 23). Both are verified on the
phone.

### 5.2 Log line prefixes

- `fb: te:` — the tear fix:
  - start‑up: measurements and the masked test
  - every 5 s: copies, rows, wait/copy µs, raced, late, timeouts, frames sent per second
- `fb: N frames in … (comparing X, scaling Y)` and `fb: from one frame to the next …` — performance
- `fb: bl:` — brightness and power:
  - DSIM state
  - panel reads (`panel as found` / `after write N`)
  - every request (`… nits -> AOR …`)
  - `display on/off`
- `fb: dpu:` — the DMA probe
- `wifi:` — redwood_exynos_pcie.c, every step (look, pins, clocks, pmu, phy, dbi, ltssm, endpoint, W2b/W3 PASS/FAIL)

### 5.3 Register and command cheat sheet

| What | Value |
|---|---|
| DECON_F | `0x19050000`: GLOBAL `0x0` (b5 IDLE, b4 RUN, b8 cmd mode), INT_PEND `0x4C` (b12 frame start, W1C), SHADOW `0x60`, TRIG `0x70` (b0 HW_TRIG_EN, **b4 mask**), FRAME_COUNT `0x2A0`, OUTFIFO line `0x474`[29:16], DATA_PATH `0x214/0x218` |
| IDMA ch c | `0x19091000 + 0x1000·c`: ENABLE `0x0`, SRC_SIZE `0x10`, BASE_Y8 `0x40`, in‑use copy `+0x800` |
| DPP ch c | `0x19041000 + 0x1000·c` |
| DSIM0 | `0x190C0000`: CONFIG `0x4C`, INTSRC `0x50` (b18 RX done), PKTHDR `0x58` (b24 BTA), PAYLOAD `0x5C`, RXFIFO `0x60`, FIFOCTRL `0x68` (b10 PH empty, b11 PH full, b9 PL full, b12 RX empty), CMD_CONFIG `0x80` |
| Panel keys | level 1 `9F A5 A5`; level 2 `F0 5A 5A` / `F0 A5 A5`; level 3 `FC 5A 5A`. Samsung registers read 0 without the key |
| Panel brightness | `B1` AOR (2 bytes), `6A` DBV (`hi lo 01`), `53` HBM (`00` off / `C0` on), `F7 0F` latch; tables in research/dpu-ref/s6e3hab_hubble*_panel*.h |
| Panel power | `28` display off, `29` display on, `10`/`11` sleep in/out |
| Wi‑Fi PCIe | see §4.4; HVC `0xc6000101` |
| Power domain unlock pattern | `smc(0x82000410, 1, <pd need_smc>, 2)` — GPU `0x18410204`, DPU `0x19010204` (not needed), AUD `0x18e10204` |

---

## 6. Files changed or created this session

**Kernel** (`hackios-xnu-kernel-main/xnu-src/xnu-8796.121.2/`):
- `iokit/Kernel/MiniIOMobileFramebuffer.cpp` (heavily extended, now 3009 lines):
  - `fbSay()`, TE/DSIM/panel defines, nits→DBV tables
  - `teSetup/teMeasure/tePresent/teReport`; `RowJob.copy/compare` + `runJob()` (shared helper threads)
  - `blSetup`, `dsimWrite`, `dsimRead`, `panelReport`, `aorRead`, `blSend`, `blWork`, `brightnessRequest`,
    `displayPower`, `setProperties` (`IOMFBBrightnessLevel`)
  - class **`AppleARMBacklight`** + `publishBacklight()`
  - selector 12 → `displayPower`
  - `dpuProbe` with the corrected DMA address, the TZPC SMC probe and `dpuSmc()`
  - per‑frame timing split
- `iokit/Kernel/MiniIOSurface.cpp`: the bpr rounding **reverted**
- `bsd/dev/redwood_exynos_pcie.c`: **new** (Wi‑Fi W2/W3, `redwood_wifi_dma_pool()`)
- `bsd/conf/files`: line 149 registers `redwood_exynos_pcie.c`
- `bsd/dev/redwood_input.c`: the extern + call `redwood_exynos_pcie_init()`
- Earlier in the session (Metal/GPU diagnostics): `bsd/dev/redwood_mali.c`, `iokit/Kernel/RedwoodMali.cpp`,
  `osfmk/arm64/loader_return.c` (log head 1 MB + ring 5 MB)

**Loader:** `xnuloader/loader.c`: `reserve_wifi_dma()`, the `/chosen` `wifi-dma-base/-size`; the boot menu
was removed earlier (`out_buf` 8 MB).

**Scripts:**
- `y2s.sh`: `payload` builds the loader with `GIC_V2=1`; `log` reads 8 MiB and strips NULs
- `tools/mk-brcmfmac.sh`: new

**LinuxKPI glue:** `wifi/lx/patches/brcmfmac/000{1,2,3}-*.patch`, `wifi/lx/include/brcmfmac/*`,
`wifi/lx/src/brcmfmac/lx_brcmfmac.c` (all new, in progress).

**Mesa/Metal/userland** (Metal phase; see 05-metal.md): `tools/mesa/air/*`,
`tools/mesa/redwood-drm/redwood_drm.c`, `tools/mesa/panfrost-faultdump.patch`, `userland/src/redwoodmetal.m`,
`redwooddrawable.m`, `inject.c`, `init.c`, `metal_drv_test.m`, `build-gpu-y2s.sh`, `mkjobs.py` (`JOB_ENV`,
QUIET), `rc.launchd.metal`, `rc.jobs.metalwall`, `rc.jobs.malidraw`.

**Docs:**
- PLANS 05, 06, 07 (rewritten), 08 (new), README
- `research/` (new)
- `fw-stock/` (new)
- this file

---

## 7. Known issues, risks, gotchas

1. **Kernel size:** about 700 KB left in the 16 MiB slot. Move root.apfs before linking brcmfmac (§3).
2. **Nothing is committed.** Commit the tree, and probably add `fw-stock/vendor.img`, `xnu-deps/` and
   `ios165/` to `.gitignore`.
3. Tearing is not 100 % gone. Pacing jitter remains. The real fix is D4 (hardware scaler, zero copy).
4. Brightness is AOR‑only. Colour is fine across the range, but it isn't Samsung's exact per‑level gamma.
   At boot CoreBrightness first sends level 0 (the darkest AOR), then the real value, so there is a brief
   dim moment.
5. `fb_flip` is untested since the address fix and must not be combined with `fb_te`.
6. HSI1 (Wi‑Fi PCIe) shares a clock unit with the **UFS the system boots from**. A wrong CMU write there can
   kill storage. The PCIe driver only touches PCIe gates, and only after the 20 s wait.
7. Never read `0x19100000` (the DPU SysMMU) or any block whose clock may be gated. Gated reads can hang the bus.
8. Never change an IOSurface's bytes‑per‑row (§4.1).
9. Agents were cancelled twice when a user message arrived mid‑task. Check partial work (`wifi/lx/src/brcmfmac`,
   PLANS sections) before relaunching.

---

## 8. `research/`: what is in it

| Folder | Contents |
|---|---|
| `dpu-ref/` | Samsung dpu30 `cal_9830` (`decon_reg.c`, `regs-decon.h`, `dpp_reg.c`, `regs-dpp.h`, `dsim_reg.c`, `regs-dsim.h`, `dsim_drv.c`, `decon_core.c`), s6e3hab panel driver + **hubble1/2/3 panel headers** (all DCS tables), `DMA_GATING_REPORT.md` (the D7 research; its TZPC hypothesis turned out unnecessary, but its address map was right), cmucal DPU excerpts |
| `abox-ref/` | `ABOX_REPORT.md` (audio register map, AP‑vs‑DSP analysis, CS35L41 init order), Samsung abox and cs35l41 sources, clk‑exynos9830 |
| `wireless-ref/` | Samsung PCIe host‑v0 driver + 9830 PHY cal, DesignWare, pinctrl, cmucal (sfr/qch/vclk), flexpmu; z3s docs, dts, patches and **their modified brcmfmac** (`z3s_brcmfmac_*`); bcmdhd custom |
| `bt-ref/` | `ios/` strings of bluetoothd/BlueTool; `samsung/` UART (`samsung.c`, `serial_s3c.h`), USI v2, cmucal, `btbcm.c`, `hci_bcm.c`, `bcm43xx.c`; `tools/` xref/cfstr helpers. The 57 MB disassembly workdir was not copied; regenerate with `/tmp/dxenv` + `llvm-objdump` |
| `gpu-ref/` | z3s docs (DISPLAY, AUDIO_DSP context in DEVLOG, GPU bring‑up), Exynos GPU platform code, Panfrost/Mesa refs |
| `dts/` | Decompiled stock y2s `dtb.img.0.dts`, `dtbo.img.0.dts`, `dtbo.img.1.dts` (the authority for addresses, pins, clocks) |
| `ref/` | Samsung drivers for touch (sec_ts), PMIC/charger (s2dos05, max77705) |

Tools set up on this Mac:
- `/tmp/dxenv`: a Python venv with DyldExtractor. Its `dyld_context.py` was patched to skip the missing
  `.symbols` subcache. Recreate it with `pip install dyldextractor setuptools 'capstone>=5'`.
- `/opt/homebrew/opt/llvm/bin/llvm-objdump`
- e2fsprogs `debugfs` (Homebrew)
- `ipsw` could not be installed: the Homebrew Command Line Tools are too old.

---

## 9. Next steps, in priority order

1. **Read the Wi‑Fi run** (§1). Then:
   - On `W2b PASS` + `W3 PASS`: continue with W4.
   - On an LTSSM failure: compare the path and history with Samsung's (`research/wireless-ref/exynos990_pcie-exynos-host-v0.c` link‑up loop).
   - If the endpoint id reads all ones: check REG_ON timing and the PHY reference clock (`PCS 0x100 = 0x87`).
   - If the HVC panics: Samsung's EL2 is absent on this boot path. Find another grant route (SMC?) or test a
     pool without grants.
2. **W4: finish brcmfmac.**
   - Write `wifi/lx/src/brcmfmac/lx_brcmfmac.c`: the PCI device on top of redwood_exynos_pcie.c (export from it:
     config read/write, the BAR0/BAR2 virtual addresses, INTx registration, the DMA pool allocator), the netdev,
     wiphy/cfg80211 registration, and firmware from `/fw/` (ship `bcmdhd_sta.bin_b1` as
     `brcmfmac4375-pcie.bin`, plus clm and a SEMCO nvram, in the root image).
   - Resolve `out/undefined.txt`.
   - Move root.apfs to 56 MiB, then link it via `xnu-build.sh` the way ath11k is linked.
   - First milestone: firmware banner `RTE (… MSGBUF) 18.41.117` in the log. Then `host_cap` → scan results.
3. **Bluetooth B1/B2:** write `bsd/dev/redwood_exynos_bt.c` (USI16 UART at 115200, BT_EN, H4 HCI reset/version;
   follow the agent spec and `research/bt-ref/samsung/samsung.c`). Then B3a (IOService `bluetooth` + kernel
   control `com.apple.uart.bluetooth`), then B3b (BlueTool stand‑in + JOB_ENV).
4. **W5:** brcmfmac under `redwood_sta` ops, so wifid lists networks and joins WPA2, then DHCP.
5. **Display D4** (hardware scaling, zero copy) → big fps gain. **D5** 120 Hz after it.
6. **Sound A2–A6** (probe, amps over I2C, UAIF2 clocks, RDMA tone, iOS audio).
7. **Metal:** capture the function constants (05-metal.md), when it is picked up again.
