# HackiOS on the Galaxy S20+ 5G (y2s, Exynos 990) — Feasibility Analysis & Execution Plan

Status: research/planning. Written after analysing the local trees and the upstream
Mu‑Silicium / HackiOS sources. Everything below is traced to a file or a public source;
open questions are marked **VERIFY**.

---

## 0. TL;DR — verdict

- **It is not crazy.** The same trick HackiOS uses on a Qualcomm phone is already proven
  on *the same silicon family*: Mu‑Silicium (Project Silicium) has a **working UEFI for
  Exynos 9830 (= Exynos 990)** on the Galaxy S20 5G (`x1s`) — display, internal storage,
  side buttons, USB and SD card all work, and it boots WinPE. `c1s` (Note 20) and `r8s`
  (S20 FE) ports also exist. **y2s is the S20+ 5G — the same SoC, the same 1440×3200 panel
  family, and (from the local trees) the same `dtb.img` as all the other Exynos‑990 units.**
  So the UEFI/boot-chain half of the problem is largely *already solved upstream*.
- **The kernel half is the real work.** HackiOS's XNU port is written for Qualcomm
  Snapdragon 778G: GICv3, Adreno, WCN6750/ath11k, Qualcomm SMEM/SMP2P/GLINK/QRTR/QMI,
  EPSS cpufreq, UFSDxe, Goodix touch, Awinic haptics, Qualcomm PMICs. The Exynos 990 is
  a **completely different SoC** and almost every device driver has to be replaced.
- **Two genuine hard problems** stand out:
  1. **Exynos 990 uses GICv2 (GIC‑400)**, while HackiOS's XNU has a **GICv3‑only**
     interrupt driver. XNU must gain a GICv2 driver. (Confirmed from the y2s device tree:
     `compatible = "arm,cortex-a15-gic"`, GICD/GICC/GICH/GICV.)
  2. **Samsung's EL2 hypervisor ("UH" / Platform Hypervisor Extensions) traps EL1 MMU
     registers.** Mu‑Silicium's own status page says the `uh` partition has to be erased for
     Windows to boot on these phones. XNU runs at EL1 and will hit the same wall. This is
     known, worked‑around (erase `uh`, recover later from the BL tar), and must be
     understood before anything else.
- **Effort:** this is a multi‑month, multi‑person research project, not a weekend port. The
  realistic first win is *UEFI + loader + XNU verbose boot* on y2s; the lock screen is a
  later milestone; GPU (Mali‑G77 via Mesa) and Wi‑Fi (Broadcom BCM4375) are the long tail.

---

## 1. What is in this workspace

| Path | What it is | Relevance to the port |
|---|---|---|
| `hackios-xnu-kernel-main/` | HackiOS XNU patches, `xnuloader` (EFI loader), UEFI port, QEMU scripts, `common/` build/run, `xl` USB driver | The thing we fork and extend |
| `hackios-xnu-userland-main/` | `init`, injected libs, launchd jobs, APFS image builder, IPSW extractor, Mesa/Metal work | **Largely device‑independent** — carries over wholesale |
| `android_device_samsung_exynos990-android-12.1/` | TWRP device tree for 6 Exynos‑990 Samsungs (x1s/c1s/c2s/r8s/z3s/**y2s**), with per‑device `prebuilt/{Image,dtb.img,dtbo.img}` | Boot layout, partition sizes, kernel config name, and the **actual y2s device trees** |
| `device_samsung_y2s-fourteen-qpr3/` | LineageOS‑style Android 14 tree for y2s (SM‑G986B) | `proprietary-files.txt` (firmware blobs), kernel config, panel/audio/display facts |
| `PLAN.md` (this file) | — | — |

Public sources analysed: `oosexgang/hackios-xnu-kernel`, `oosexgang/hackios-xnu-userland`,
`Project-Silicium/Mu-Silicium` (README, `Status.md`, `Platforms/Samsung/x1sPkg`,
`Silicon/Samsung/S5E9830Pkg`, `BootShim/`), `Project-Silicium/Guides`.

---

## 2. How HackiOS boots today (redwood / QEMU)

```
fastboot boot  ──► UEFI (Mu‑Silicium) ──► xnuloader (EFI app, stays resident)
                                             │  provides a USB link ("fastboot" protocol),
                                             │  watchdog, receives the kernel log
                                             ▼
        XNU (BCM2837 config as a generic base + platform_redwood)
          ├─ GICv3, virtual timer, PSCI SMP (via SMC/HVC), return-to-loader
          ├─ APFS (images in RAM, iOS volume on flash) + ramfs
          ├─ driver replacements: IOSurface, IOMobileFramebuffer, AppleKeyStore, net stack
          ├─ input: /dev/redwood-touch  ◄─ Goodix GT9916 (phone) / virtio-input (QEMU)
          ├─ GPU Adreno 642L (Mesa/freedreno), Wi‑Fi WCN6750 (ath11k+LinuxKPI),
          │  audio ADSP, haptics Awinic, camera, UFS via the loader's UFSDxe
          ▼
        iOS 16.5 launchd → … → backboardd → SpringBoard → apps
```

Device‑specific surface area is exactly three layers: **(1)** the UEFI port, **(2)** the
loader's `struct platform` in `xnuloader/loader.c`, **(3)** the `redwood_*` kernel drivers.
Everything above (APFS, ramfs, mini‑IOSurface/IOMobileFramebuffer/AppleKeyStore/netstack,
minicrypto, AMFI/Sandbox stubs, the userland, the APFS builder) is generic.

`docs/PORTING.md` is the upstream porting guide and is the canonical checklist.

---

## 3. The gap: redwood (SD 778G) → y2s (Exynos 990)

Facts pulled from the actual y2s device tree (`y2s/prebuilt/dtb.img` + `dtbo.img`,
parsed locally) and the Mu‑Silicium Exynos packages.

| Subsystem | redwood (HackiOS today) | **y2s (Exynos 990)** | Work |
|---|---|---|---|
| SoC | Qualcomm SM7325, 4×A78+4×A55 | **Exynos 990 (S5E9830)**, 3 clusters: 4×A55 (`ananke`, 0‑3) + 2×A76 (`enyo`, 4‑5) + 2×Mongoose M5 (`lion`, 0x100‑0x101) | loader `platform_y2s`, cluster/pairing logic |
| **Interrupts** | **GICv3** (`arm,gic-v3`) | **GICv2 / GIC‑400** (`arm,cortex-a15-gic`; GICD `0x10101000`, GICC `0x10102000`, GICH `0x10104000`, GICV `0x10106000`) | **NEW GICv2 driver in XNU** (biggest kernel change) |
| PSCI | SMC | `arm,psci-1.0`, `method = "smc"` → `psci_hvc=0` | small |
| Timer freq | (Qualcomm) | **26 MHz**, `HAS_CNTFRQ_EL0_EMPTY_ERRATA=1`, physical timer off | loader/platform |
| Boot chain | `fastboot boot` (nothing flashed) | **Samsung/Odin**, AVB, `uh` hypervisor partition | **flashing flow, UH handling** |
| UEFI | Mu‑Silicium `redwoodPkg` + Qualcomm DXEs from `xbl.img` | **Mu‑Silicium already has an Exynos 9830 port** (`x1sPkg`, active) | copy `x1sPkg`→`y2sPkg`, adjust |
| Storage | UFS via Qualcomm `UFSDxe` | `samsung,exynos-ufs` @ `0x13100000`; UniPro `0x13180000` | use Silicium `PlatformUfsLib`, then a kernel UFS path |
| Display | `IOMobileFramebuffer` on the UEFI GOP at `0xE1000000` | Exynos **DECON** `0x19050000` + **DSIM** `0x190C0000`; panel **s6e3hab "hubble"**, 1440×3200; UEFI reserved FB @ `0xF1000000` | keep GOP‑based MiniIOMobileFramebuffer, verify FB base |
| Touch | Goodix GT9916 over bit‑banged SPI | **Samsung `sec,sec_ts` @48 over HSI2C** (`tspi2c`) + IRQ GPIO | new `bsd/dev/*_touch.c` |
| GPU | **Adreno 642L** (Mesa freedreno) | **Mali‑G77 MP11** (`arm,mali` @ `0x18500000`) | Mesa **Panfrost/PanVK** instead of freedreno |
| Wi‑Fi | WCN6750 + ath11k + LinuxKPI/QMI/QRTR/GLINK | **Broadcom BCM4375** (`samsung,bcm43xx` / bcmdhd / brcmfmac) | new Broadcom driver; **drop the whole ath11k/LinuxKPI path** |
| Bluetooth | (not working yet) | BCM4375 (bcmdhd) | later |
| Audio | Qualcomm LPASS/ADSP, WCD938x | Exynos **A‑Box** (`abox@18c50000`) + **Cirrus CS35L41** amps | new audio path |
| Haptics | Awinic AW86927 (I2C fw) | **Cirrus CS40L25** (`cs40l25@40`) | new driver |
| Buttons/PMIC | PMK8350/PM7325 (SPMI) | Samsung **S2MPS19/S2MPS22/S2DOS05/S2MPB02** | new driver |
| Sensors | — | BCM4773 GNSS, SX9360 grip | later |
| Camera | OV02B10/S5KHM2 | Exynos camera (`camera.exynos990.so`) | late/optional |
| Generic (reusable) | APFS, ramfs, mini‑IOSurface/IOMobileFramebuffer/AppleKeyStore/IONetworkStack, minicrypto, AMFI/Sandbox stubs, `redwood_input/image/sparse/net/apple80211`, virtio‑*, big.LITTLE pairing | same | none |

---

## 4. Critical findings & the hard problems

### 4.1 UEFI: half the problem is already done upstream ✅ / ⚠️
`Project-Silicium/Mu-Silicium` has:
- `Platforms/Samsung/x1sPkg` (**State: Active**) for the S20 5G — display ✅, internal
  storage ✅, side buttons ✅, USB device ✅, SD ✅, WinPE boot ✅.
- `Silicon/Samsung/S5E9830Pkg` — GIC `0x10101000/0x10102000`, 8 cores / 3 clusters,
  timer 26 MHz, GPIO/SPEIEDY/PMIC/UFS/MSHC/USB libraries, S2MPU + HARX (`PcdHasSecurePgtbl`,
  `PcdHasHarx`), and the full Exynos‑990 memory map.
- `x1sPkg/Library/MemoryMapLib` gives the key boot addresses: **UEFI FD `0xE8000000`
  (2 MB)**, DXE heap `0xE8200000`, UEFI stack `0xEBE00000`, **Display reserved
  `0xF1000000`**, EL2 reservations `0x83C80000`/`0xC1400000`, S2MPU table `0xC1800000`.

So the plan is **not** to write an Exynos UEFI from scratch — it is to add `y2sPkg`
(copy of `x1sPkg`, same 1440×3200 FB, SMBIOS updated) and, if needed, borrow panel/DDI
data from the y2s `dtbo`. `c1sPkg` (Note 20, same SoC) is a second reference.

### 4.2 GICv2 — the biggest kernel change ⚠️⚠️
`y2s` DTB: `/interrupt-controller@10100000` → `compatible = "arm,cortex-a15-gic"` with 4
register windows (GICD/GICC/GICH/GICV). That is **GICv2**.
HackiOS's XNU carries `osfmk/arm64/gic_v3_irq.c` and builds with `GENERIC_GIC_V3_IRQ=1`;
the loader builds an `/arm-io/gic` node with `gic_dist`/`gic_redist` (a GICv3 concept).
**There is no GICv2 path.** We must add one: GICD MMIO + banked GICC CPU interface,
SGIs for CPU signalling (the code uses SGI 0/1 already), PPI 27 for the virtual timer.
GICv2 is simpler than v3, well documented, and the loader already abstracts `gic_dist`
(we set `gic_redist = 0` / unused). This is real but bounded work.

### 4.3 Samsung hypervisor (UH) + boot/flash method ⚠️⚠️
- Samsung has no `fastboot boot`. Mu‑Silicium on Samsung is loaded as the **boot
  partition kernel**: the `BootShim.S` has a `REQUIRES_KERNEL_HEADER` mode that prepends an
  ARM64 Linux `Image` header (`ARM\x64`) and copies the UEFI FD to `FD_BASE`. That is
  flashed via Odin/Heimdall/TWRP‑`dd` to `boot` (recoverable by restoring the stock image).
  → HackiOS's `xl enter`/`fastboot boot` step is replaced by a **flash step**; the loader's
  resident USB link can still be used to upload the system image. **VERIFY** the exact
  y2s flow.
- The **Platform Hypervisor Extensions partition (`uh`)** runs an EL2 hypervisor that
  **traps EL1 MMU registers**. Silicium's own note: erase it (`dd if=/dev/zero of=…/uh`)
  for Windows; Android then won't boot until restored from the BL tar. XNU is an EL1 OS →
  same issue. Must confirm whether the Silicium boot path already neutralises UH on Exynos,
  or whether we erase it during bring‑up. Also note `PcdHasSecurePgtbl`/`PcdHasHarx` are
  TRUE for Exynos — secure page tables mean extra SMC/TZ interaction to expect.

### 4.4 CPU topology / MPIDR
`y2s` CPUs: `cpu@0..3` (A55), `cpu@4..5` (A76), `cpu@0x100,0x101` (M5). HackiOS's
`new-device.sh` assumes Qualcomm's "one Aff1 per core" numbering (`r >> 8`); on Exynos the
big cluster differs only in `Aff0` while `Aff1` selects the cluster, so the auto‑generated
`small_cores` mask is wrong. Set `platform_y2s` by hand and confirm PSCI `CPU_ON` affinity
arguments. `core_shift`/`small_cores` and the big.LITTLE pairing
(`ml_pair_slow_processors`) need care for **3 clusters** (HackiOS currently patches
`MAX_CPU_CLUSTERS 1`).

### 4.5 GPU (Mali‑G77) — long tail ⚠️
HackiOS's GPU work is firmly Adreno/freedreno. y2s is **Mali‑G77 MP11** (Valhall).
Mesa's Panfrost supports Valhall (G77) and is the natural target, and HackiOS's
`eagl.c` / `tools/mesa` / AIR‑frontend work is reusable in principle — but the driver
integration (`RedwoodGPU.cpp` speaks Linux msm DRM) becomes a **Panfrost/KMS** integration.
This is the single largest "unknown‑unknown" and belongs late in the plan (after the lock
screen, mirroring how redwood did it).

### 4.6 Wi‑Fi (Broadcom BCM4375) ⚠️
The entire `wifi/lx/` layer + `redwood_wifi.c`/`wlfw`/`qmi`/`qrtr` + ath11k is useless for
y2s. A Broadcom FullMAC path (brcmfmac‑style) is a fresh driver. Not needed for a first
boot.

---

## 5. Recommended strategy

1. **Stand on Mu‑Silicium's Exynos‑990 work.** Do not re‑create the UEFI; add `y2sPkg`
   from `x1sPkg`. Get Silicium itself booting on y2s first (display + UFS), so we know the
   boot chain and the UH question are settled before touching XNU.
2. **Bring XNU up in QEMU first, then on a "GICv2 machine".** HackiOS runs in QEMU (`virt`,
   GICv3). To de‑risk the Exynos kernel work cheaply, add a **GICv2 target to QEMU**
   (`-machine virt,gic-version=2` is *not* how QEMU virt works; use a GICv2‑capable
   machine/monitor, e.g. `-machine virt` with `gic-version=2` on real hw isn't possible —
   alternative: `qemu-system-aarch64 -M virt -cpu cortex-a57 -machine gic-version=2` works
   on `virt` **only if** the VM exposes GICv2; otherwise use the `versatile`/`raspi` models
   or `-machine sbsa-ref`). **VERIFY** the cheapest GICv2 QEMU model; the point is to have a
   fast edit‑compile‑boot loop for the new GICv2 + timer + PSCI code **without a phone**.
3. **Milestone‑driven, "no extras first"** — exactly as `docs/PORTING.md` advises: the core
   (GICv2, timer, PSCI SMP, loader‑return, APFS/ramfs) should reach the lock screen with
   only **framebuffer + touch + CPU**. GPU/Wi‑Fi/audio/camera come after.
4. **Keep the two repos in sync with upstream.** Fork both HackiOS repos; keep the userland
   untouched. Every device‑specific change goes behind a `y2s` profile so the redwood path
   still builds.

---

## 6. Phased plan

Each milestone has an explicit **exit criterion**. Do not start M(n+1) before M(n) is green.

### M0 — Environment & baseline (days)
- Fork `hackios-xnu-kernel` + `hackios-xnu-userland`; run `common/setup.sh`.
- Build + run **QEMU `qemu-virt`** to reproduce the lock screen (proves toolchain/IPSW
  fetch/userland build work before any Exynos work).
- Set up `theos`/`iPhoneOS16.5.sdk` + clang/lld so the kernel actually compiles (otherwise
  we're testing `prebuilt/` — useless for our changes).
- ✔ *Exit:* local QEMU session reaches SpringBoard; local `xnu-build.sh` produces a kernel.

### M1 — Get Mu‑Silicium running on y2s (no HackiOS yet) (days–weeks)
- Clone Mu‑Silicium; build `x1s` and try it on the y2s (same SoC, same DTB). **VERIFY**
  display (panel is a different DDI, likely fine at first), UFS, buttons, USB.
- Create `y2sPkg` = `x1sPkg` with corrected SMBIOS; if the panel misbehaves, port the
  `s6e3hab_hubble2` DDI/panel data from the y2s `dtbo`.
- **Settle the boot method**: build the boot image (`BootShim` with `REQUIRES_KERNEL_HEADER`,
  `FD_BASE=0xE8000000`, `FD_SIZE=0x200000`), flash via the Samsung flow; document the
  recovery path.
- **Settle UH**: confirm whether `uh` must be erased, and whether the Silicium boot path
  already disables it. This determines whether XNU can run at EL1 unchanged.
- ✔ *Exit:* y2s shows the Silicium UEFI (boot menu / WinPE) and USB works.

### M2 — `xnuloader` platform + resident loader on y2s (weeks)
- Add `platform_y2s` to `xnuloader/loader.c` (`common/devices/y2s.platform.c`):
  `io_base≈0x02000000, io_size≈0x19000000` (one non‑zero window over peripherals),
  `gic_dist=0x10101000`, `gic_redist` unused, `psci_hvc=0`, `cores=8`, cores/M5 mapping,
  `small_cores` = A55 mask (hand‑computed), `fb_base=0xF1000000` (**VERIFY** from runtime
  "display … at" print).
- Remove/de‑gate the Qualcomm‑only branches: `charger_start()` (`PLAT==redwood`),
  `wdt_arm()` (Qualcomm `VIRT_WDT` SMC), and the redwood UFSDxe assumptions.
- New `common/devices/y2s.conf`; make `common/build.sh`/`mkxnuimg.py` emit a **boot‑image
  for flashing** (not `fastboot boot`) for `KIND=phone` on Samsung.
- ✔ *Exit:* flashing the y2s boot image starts the UEFI, our loader runs, `./xl log`
  (or the loader's USB link) shows the loader's screen and log.

### M3 — XNU kernel core on Exynos 990 (weeks–months) — **the crux**
- **Add a GICv2 driver** (`osfmk/arm64/gic_v2_irq.c` + loader DT node) and select it for
  y2s. Keep GICv3 for redwood/QEMU.
- Timer: virtual timer only (already the case); disable/adjust `BCM2837_CORE_TIMER_ROUTING`.
- PSCI SMP: SMC method; verify `CPU_ON` affinity encoding for the 8 Exynos cores;
  re‑evaluate `MAX_CPU_CLUSTERS` and the slow‑core pairing for 3 clusters.
- `loader_return.c`/`machine_routines.c`: drop the Qualcomm virtual‑watchdog SMC; keep the
  return‑to‑loader log channel; re‑check the `__TEXT/__DATA` cache clean for Exynos (should
  be fine or better).
- Keep the generic stand‑ins (APFS, ramfs, mini‑IOMobileFramebuffer on the GOP, minicrypto,
  AMFI/Sandbox).
- ✔ *Exit:* verbose kernel boot (`-v`) prints on y2s and reaches the point where it needs
  touch/display (i.e. it doesn't die on interrupts/SMP).

### M4 — Storage + userland boot to lock screen (weeks–months)
- Storage: get a block device to XNU. Easiest first: keep the **images in RAM** and let the
  loader serve block I/O (as redwood does with its loader), or reuse Silicium's UFS stack.
  Replace `redwood_ufs.c`/`redwood_ufshc.c` with an Exynos UFS path (`samsung,exynos-ufs`).
- Build `root.apfs`/`system.apfs` with `hackios-xnu-userland` (unchanged) and upload.
- Framebuffer: confirm `MiniIOMobileFramebuffer` paints via the UEFI GOP at `0xF1000000`.
- ✔ *Exit:* **iOS 16.5 lock screen on y2s** (the equivalent of HackiOS's first phone win).

### M5 — Input & basic interaction (weeks)
- Touch: new `bsd/dev/` driver for **`sec,sec_ts` over HSI2C** feeding
  `/dev/<name>-touch` (reuse `redwood_input.c` as the model). Find its interrupt number
  from the DTB.
- Buttons: power/volume via the **S2MPS PMIC PON** (new driver modelled on
  `redwood_buttons.c`).
- ✔ *Exit:* swipe to unlock, use Settings/Clock/Calendar on y2s.

### M6 — GPU, audio, Wi‑Fi, then the rest (long tail)
- **GPU**: Mesa **Panfrost/PanVK** for Mali‑G77; port `eagl.c` + the AIR/Metal work from
  freedreno to Panfrost; new kernel `RedwoodGPU`‑analogue speaking Panfrost/KMS.
- **Audio**: Exynos **A‑Box** + **CS35L41**; new CoreAudio output device.
- **Wi‑Fi**: Broadcom **BCM4375** FullMAC driver (drop ath11k/LinuxKPI/QMI/QRTR entirely
  for y2s).
- **Haptics** CS40L25, **sensors**, **camera** (optional), **Bluetooth**.

---

## 7. Concrete first changes (file‑by‑file)

Kernel repo (`hackios-xnu-kernel`):
- `xnuloader/loader.c` — add `platform_y2s`, `#if`/runtime‑gate the Qualcomm‑only paths
  (charger, VIRT_WDT), add `&platform_y2s` to `platforms[]`.
- `common/devices/y2s.conf` + `y2s.platform.c` — new profile (from `_template.conf`).
- `common/build.sh`, `mkxnuimg.py`, `common/run.sh` — a Samsung "flash boot image" path
  (vs `fastboot boot`), `KIND=phone` + `FLASH=odin|heimdall|dd`.
- `xnu/files/osfmk/arm64/gic_v2_irq.c` (new) + `xnu/redwood.patch`/`osfmk/conf/files.arm64`
  wiring; select GICv2 vs v3 per platform.
- `xnu/files/bsd/dev/` — start `exynos_ufs.c`, `exynos_touch.c`, `exynos_buttons.c`
  (model on the `redwood_*` files); leave `redwood_*` intact behind the redwood profile.
- `uefi/` — add `y2s` to the Silicium port config; `Resources/DTBs/y2s.dtb`; boot‑shim
  addresses (`FD_BASE=0xE8000000`, `FD_SIZE=0x200000`).

Userland repo (`hackios-xnu-userland`): **no changes needed for M0–M5**; only M6
(GPU/audio plugins) touches it.

---

## 8. Risks & unknowns (and how to close them)

| # | Risk / unknown | How to resolve | Blocking? |
|---|---|---|---|
| R1 | Exact y2s boot/flash flow (Odin vs Heimdall vs TWRP dd) and BL requirements | Read Silicium Samsung guides (currently sparse) + Discord; experiment on a **spare** y2s with stock images saved | Blocks M1 |
| R2 | `uh` hypervisor trapping EL1 MMU regs → XNU won't run at EL1 | Erase `uh` (recoverable), or have the UEFI disable UH; test with Silicium WinPE first | Blocks M3 |
| R3 | GICv2 driver effort in XNU | Write it; validate in a GICv2 QEMU/emulator loop first | Blocks M3 |
| R4 | MPIDR/cluster mapping & PSCI affinity encoding | Experiment; derive from DTB + PSCI tests | Blocks M3 |
| R5 | UFS path into XNU (Silicium lib vs kernel driver) | Start with RAM images + loader block I/O | Blocks M4 |
| R6 | Mali‑G77 GPU integration (Panfrost/KMS) | Treat as its own project; follow redwood's `gpu=0` first | M6 |
| R7 | Broadcom Wi‑Fi driver | Fresh driver; skip until later | M6 |
| R8 | Samsung AVB / KG / "OEM lock" states bricking | Save stock BL/boot tariffs; use known unlock procedure | M1 |
| R9 | Secure page tables (`PcdHasSecurePgtbl`/HARX) → extra TZ/SMC dance | Study Silicium's S2MPU handling | M1/M3 |

**De‑risking rule:** whenever a step needs the phone, first reproduce it in QEMU. The
biggest cost of this project is phone‑in‑hand cycles; minimise them.

---

## 9. Tooling / environment

- macOS dev box for editing (current). **Builds need Linux x86‑64** (Mu‑Silicium and the
  XNU cross‑build are Linux‑oriented). Use a Linux VM/box or WSL2.
- `theos` + `iPhoneOS16.5.sdk`, `clang lld python perl`, `7zip ≥23`, `lz4`, `qemu-system-aarch64`,
  `dtc`, git. `adb`, `fastboot`, `heimdall` and/or Odin for the phone.
- An **unlocked Exynos y2s** (SM‑G986B), preferably a spare, with stock firmware archived.
- QEMU with a GICv2‑capable model for the interrupt‑driver bring‑up (**VERIFY** best model).

---

## 10. Effort estimate (rough)

| Phase | Optimistic | Realistic |
|---|---|---|
| M0 environment/baseline | 2–4 days | 1 week |
| M1 Silicium on y2s | 1–2 weeks | 3–6 weeks |
| M2 loader platform + flash flow | 1–2 weeks | 3–5 weeks |
| M3 XNU core (GICv2 etc.) | 3–6 weeks | 2–4 months |
| M4 lock screen | 3–6 weeks | 2–4 months |
| M5 touch/buttons | 1–3 weeks | 1–2 months |
| M6 GPU/audio/Wi‑Fi/… | months | many months |

Total to a **lock screen**: plausibly ~4–8 months of focused work by someone strong in
low‑level ARM/XNU; the full "usable iOS" milestone is a much longer tail (exactly like
HackiOS on redwood, which is itself still incomplete).

---

## 11. Immediate next steps (this week)

1. **Fork** both HackiOS repos; get QEMU `qemu-virt` booting the lock screen locally (M0).
2. **Clone Mu‑Silicium**; build `x1s`; identify the exact Samsung flashing procedure and the
   `uh` situation (R1, R2). Confirm x1s UEFI displays on a y2s.
3. **Prototype GICv2 XNU support** in a QEMU loop (R3) — highest‑leverage kernel de‑risking.
4. Draft `common/devices/y2s.conf` + `y2s.platform.c` and the `platform_y2s` entry.
5. Open a tracking issue in the fork with the M0–M6 checklist.

---

### Appendix A — key addresses (from the local y2s trees + Mu‑Silicium)

```
GICD 0x10101000  GICC 0x10102000  GICH 0x10104000  GICV 0x10106000   (GICv2)
RAM base 0x80000000                                     (PcdSystemMemoryBase)
UFS HCI 0x13100000   UFS UniPro 0x13180000   MSHC/SD 0x132E0000
DECON 0x19050000   DSIM 0x190C0000   DSI DPHY 0x190E0000
Mali 0x18500000    Clock controller 0x1A330000   PMU 0x15860000   Chip ID 0x10000000
Pinctrl 0x10430000 0x10730000 0x13040000 0x13C30000 0x15580000 0x15850000 0x15C30000
UEFI FD 0xE8000000 (2 MB)   DXE heap 0xE8200000   UEFI stack 0xEBE00000
Display reserved 0xF1000000 (0x1194000)   EL2 0x83C80000 / 0xC1400000   S2MPU 0xC1800000
timer 26 MHz, 8 cores / 3 clusters (4×A55, 2×A76, 2×M5)
PSCI: arm,psci-1.0, method=smc
```
