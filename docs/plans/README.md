# PLANS — y2s (Galaxy S20+, Exynos 990) iOS port

Working plans for the next phase, after touch was brought up (2026‑10‑24).
Each plan is a separate file. Every step says what to change, how to measure it,
and what counts as done.

| File | Topic | Priority |
|---|---|---|
| [00-foundations.md](00-foundations.md) | iteration loop, logging without panics, measurement, source hygiene | **first**: everything else depends on it |
| [01-performance.md](01-performance.md) | why SpringBoard is slow and how to speed it up | **high** |
| [02-buttons.md](02-buttons.md) | power / volume keys | high (small, self-contained) |
| [03-battery-usb.md](03-battery-usb.md) | battery gauge, charger state, USB | medium |
| [04-gpu.md](04-gpu.md) | Mali‑G77: power, Panfrost UAPI in XNU, Mesa | high, long |
| [05-metal.md](05-metal.md) | Metal on the Mali: history to 25 Oct | superseded by 10 |
| [06-wireless.md](06-wireless.md) | Wi‑Fi (BCM4375 over PCIe), Bluetooth (UART), cellular | Wi‑Fi done; Bluetooth transport done, profiles untested |
| [07-display.md](07-display.md) | tearing, brightness, frame rate, 120 Hz | D4 done; frame pacing in 10; 120 Hz open |
| [08-audio.md](08-audio.md) | speakers through ABOX + CS35L41 amps | open: RDMA3 reads no frames |
| [09-standalone.md](09-standalone.md) | iOS as the only OS: menu, USB/Wi-Fi logs, flash root, HFS+ data | done (warm boot after a return open) |
| [10-mali2metal.md](10-mali2metal.md) | **mali2metal**: Metal from evidence; the m2m channel and the loop | **current** |

Current state (2026‑10‑27): all eight cores, governor, thermal steps and idle power-down done; frame pacing (20-25 % of frames late) and sound open; see the run log in [10-mali2metal.md](10-mali2metal.md). Order at the end of 2026‑10‑25: Wi‑Fi (read the W2/W3 run → W4 → W5) and Bluetooth (B1/B2 → B3) in parallel → display D4/D5 → sound (A2 → A6). Full handover: [../history/PROGRESS2510.md](../history/PROGRESS2510.md).

## Status board

| ID | Step | State |
|---|---|---|
| F1 | Export kernel work into `xnu/` (`export.sh`) | **done**: `y2s.sh kernel` runs it every build |
| F2 | One-command ship script (`hackios-xnu-kernel-main/y2s.sh`) | **done**: payload splice verified byte-identical |
| F3 | Log without a panic: five‑finger / Vol‑Down+Power 3 s → loader saves log | **done**: loader now saves before printing |
| F4 | Perf instrumentation (`bsd/dev/redwood_perf.c`, `perf=1`) | **built**: per-core MHz at boot + every 60 s |
| F5 | Doc corrections (A55 boot core, etc.) | planned |
| P1 | Baseline: CPU clock per core, frame times, boot time | **done** (see 01 §4–5) |
| P2 | Multi‑core | **done**: 8/8 cores (`cpumask=0xff`, M5s with `jit_flush=1`) |
| P3 | Render fewer pixels (`fbscale=2`, filter off) | planned |
| P4 | Faster swap path (damage‑only copy) | **done**: `fb_damage` ~80 ms → ~10 ms/frame |
| P5 | CPU DVFS through ACPM | **done**: governor, Samsung thermal steps, touch booster, `cpuidle=1` |
| P6 | Userland: reduce transparency/motion, trim daemons | planned |
| P7 | RELEASE kernel | planned |
| B1–B4 | Buttons | B1–B3 **done** (verified on phone); B4 (real screen-off) planned |
| U1–U4 | Battery | **done**: `redwood_max77705.c`, `bms=1` (default) |
| U5–U6 | USB device mode, charging policy | planned |
| G1–G2 | GPU power + first job | **done**: G77 ran a WRITE_VALUE job through its MMU (`mali=5`) |
| G3–G7 | GPU driver, panfrost UAPI, Mesa, CoreAnimation on the GPU | **done**: `mali=1` (default) |
| G8–G10 | GPU perf (IRQ, DVFS), display flip (tearing), Metal | **done**: job interrupt, GPU governor, Metal for CoreAnimation (10-mali2metal.md) |
| D2b, D6, D8 | Tear‑free copy under a masked TE trigger (`fb_te=1`); brightness (AOR dimming via an `AppleARMBacklight` service) and screen off/on through the DSIM (`fb_bl=1`) | **done** (verified on the phone 2026‑10‑25) |
| D7 | DPU DMA registers | **done**: IDMA is at `0x19091000` and readable; no unlock needed |
| D4, D5 | Zero‑copy hardware scaling via a DPP; 120 Hz (FHD mode set) | D4 **done** (`fb_scaler=1`); D5 planned |
| W1 | Wi‑Fi/BT firmware pulled from the stock AP tar (`fw-stock/`) | **done** |
| W2, W3 | PCIe link (`redwood_exynos_pcie.c`, `wifi_pcie=1/2/3`); 32 MB DMA pool below 4 GB from the loader + HVC grants | **done** |
| W4, W5 | BCM4375 msgbuf driver, in-kernel WPA2, Apple80211 for wifid | **done**: WPA2, DHCP, Safari; signal and noise reported |
| B3 | How iOS bluetoothd attaches (kernel control socket + IOService `bluetooth`, BlueTool stand‑in) | **solved** (design in 06-wireless.md) |
| B1, B2 | USI16 UART + HCI bring‑up driver | **done**: patchram, 3 Mbaud, `bluetoothd` attaches |
| A1–A6 | Sound without the DSP: RDMA → UAIF2 → CS35L41 (08-audio.md) | programmed; RDMA3 reads no frames |
| M0–M5 | Metal on the Mali (05-metal.md) | the desktop renders on Metal (run 6 on); continued as mali2metal (10-mali2metal.md) |

## Key facts that changed in this round

- **The boot core is a Cortex‑A55** (little core), not an A76. `MIDR 0x412FD050` has
  part number `0xD05` = A55. Topology (DTB): cpu0–3 A55 (MPIDR 0–3), cpu4–5 A76
  (4, 5), cpu6–7 Mongoose M5 (`0x100`, `0x101`).
- The loader adds `cpus=1` unless the payload asks for more. **iOS currently runs on
  one little core.**
- No GPU: CoreAnimation's **software rasterizer** draws a 1125×2436 (iPhone X) frame.
  The kernel then **bilinear‑scales the whole frame** to the 1440×3200 panel on every
  swap (`MiniIOMobileFramebuffer.cpp`).

## Watch

- The kernel is **16,005,600 bytes**, and its slot in `recovery` is 16 MiB (32 → 48 MiB,
  where `root.apfs` starts). That leaves ~750 KB. Before it runs out, move `root.apfs` to 56 MiB
  (`--roff 3800000` in `y2s.sh payload` and `pack`). `recovery` is 65.8 MiB, so it fits.
