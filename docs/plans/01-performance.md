# 01 — Performance: why SpringBoard is slow, and the plan to fix it

## 1. The pipeline as it runs today

```
SpringBoard / apps  ── CA commits ──▶  backboardd (CoreAnimation render server)
                                          │  CA's own SOFTWARE rasterizer (no GPU: inject.c forces it)
                                          ▼  draws 1125×2436 (iPhone X, 3× scale) into an IOSurface
                                       IOMobileFramebuffer swap (kernel, MiniIOMobileFramebuffer.cpp)
                                          │  show(): every layer, every swap, the WHOLE frame
                                          ▼  bilinear-scaled 1125×2436 → 1440×3200 (×1.28 / ×1.31)
                                       framebuffer 0xF1000000 (write-combined, scanned out by DECON)

ALL OF IT ON ONE CPU: core 0 = Cortex-A55 (little core), at whatever clock sboot left it.
```

Evidence:

- The payload cmdline is just `touch_ts=1`. The loader (`loader.c`, "one processor
  unless the payload asks for more") then adds **`cpus=1`**, and the kernel starts on
  the core the loader runs on, **MPIDR 0 = Cortex‑A55**. The topology comes from DTB
  `cpus`: `ananke` = A55 ×4, `enyo` = A76 ×2, `lion` = Mongoose M5 ×2.
- No CPU frequency driver: `redwood_cpufreq.c` is Qualcomm‑only. Clocks are what
  sboot set, which is unknown and measured in P1.
- `MiniIOMobileFramebuffer.cpp` comment: scaling one frame "takes one processor some
  9 ms". That was measured upstream on a **2.4 GHz Cortex‑A78** with a **1080×2400** panel.
  Here the panel has 1.78× the pixels and the core is an A55 (roughly ⅓ of the
  per‑clock throughput, and a lower clock). Expect about **50–100 ms per frame just to
  copy the picture**, on the same single core CoreAnimation needs for drawing.
- `fb_threads=3` (default) starts three scaling threads that time‑slice on one core.
  That only adds switching overhead.
- CoreAnimation's software renderer is expensive for **blurs** (Control Centre, the
  dock, folders, notification backgrounds) and full-screen animations. These are
  everywhere in iOS 16.

So the frame rate is bounded by (CA draw + full-frame scale) on one slow core. Every
input event, app launch, and daemon competes for the same core.

## 2. Strategy, in order of value per risk

| Step | What | Expected gain | Risk | Cost |
|---|---|---|---|---|
| P1 | **Measure**: clock per core, frame stats, boot time | (decides P5) | none | small |
| P2 | **More cores, then big cores** | ×2–×6 | medium: SMP never run on this hardware | boot args + debugging |
| P3 | **Fewer pixels**: `fbscale=2`, `fb_threads` = cores | CA work ÷2.25 | low | boot args |
| P4 | **Swap path**: copy only rows that changed | ≥×3 on mostly‑static screens | low | ~200 lines kernel |
| P6 | **Userland**: Reduce Transparency/Motion, trim daemons | large in blur-heavy UI | low | image defaults |
| P5 | **CPU DVFS** (ACPM) if clocks are low | up to ×2 | high | research + driver |
| P7 | RELEASE kernel instead of DEVELOPMENT | 5–15 % | low | build config |

## 3. Steps

### P1 — Baseline (do first, no code risk)

1. Build the F4 perf module. It prints the **MHz of each core** (cycles over a fixed arch‑timer
   interval), the boot milestones, and a periodic `perf:` line.
2. Boot with the current settings plus `perf=1`. The framebuffer driver already logs
   every 5 s: `N frames in M ms (F a second), U us each in the driver …`.
3. Record:
   - seconds to lock screen and to home screen
   - frames/s while scrolling the home screen
   - µs per swap in the driver
   - each core's MHz

Results go into the table at the bottom.

### P2 — Multi-core, then the big cores

The loader and kernel already support it (`cpus=N`, `cpumask=M`, `bootcpu=N`;
upstream ran `bootcpu=7 cpus=8`). It has **never been run on Exynos**. The parts
involved:

- PSCI `CPU_ON` through SMC (`psci-method smc` in the loader's DT; the platform
  has `psci_hvc = 0`, which is right for Exynos sboot/EL3).
- GICv2 SMP: `gic_v2_irq.c` sends IPIs via `GICD_SGIR`. Each core discovers its own CPU
  interface number from `GICD_ITARGETSR` (`gic_cpuif_self()`). The Exynos wiring has
  not been verified.
- Per-core arch timer PPIs, and CNTFRQ = 0 on every core (the kernel uses the DT's 26 MHz).
- The core that runs the loader: with `bootcpu=N` the loader stays on core 0 and
  watches. That core is then not the kernel's, so `cpus=8` gives 7 cores.

Order of experiments (one per run):

| Run | Cmdline add | Tests |
|---|---|---|
| P2a | `cpus=2` | PSCI CPU_ON of one A55 in the same cluster; IPIs |
| P2b | `cpus=4` | the whole little cluster |
| P2c | `bootcpu=6 cpus=1` | kernel on an M5 (loader on core 0), single core |
| P2d | `bootcpu=7 cpus=8` | everything: M5 boot core, then M5, A76 ×2, A55 ×3 |

If a run hangs, read the log (F3). The likely culprits are the GIC CPU‑interface map
and the PSCI return codes printed by the loader ("core %x: the firmware answers %d").

**Thermal:** there is no thermal driver. The M5 cores at sboot's clock are probably
moderate, but watch the phone's temperature in long runs. `thermald.c` exists in
userland (upstream) for reference. The Exynos TMU at `0x10…` is a later item.

### P3 — Render fewer pixels

`fbscale=2` (already implemented): CoreAnimation draws the same 375×812 points at
2 pixels per point, **750×1624**, which is 2.25× fewer pixels. The kernel then scales it up
to the panel. Text is a little softer. The swap cost is unchanged because it is bound by
panel pixels, so this must be combined with P4.

`fb_threads=N`: set it to the number of fast cores the kernel has (1 with `cpus=1`).

`fb_filter=0` (nearest neighbour) halves the swap cost at some quality loss. Test it
only for measurement; keep bilinear if P4 makes the swap cheap.

### P4 — Swap path: only copy what changed

Today `show()` → `drawLayer()` → `drawRows()` scales **every row of every layer on
every swap**, even when CoreAnimation changed only a clock digit.

Plan:

1. **Shadow of the last shown source.** For layer 0 at full‑screen size, keep a
   kernel copy (cached RAM, 1125×2436×4 ≈ 11 MB) of the source rows last put on
   the panel.
2. On each swap, `memcmp` each source row with its shadow row. This reads cached memory
   at memory bandwidth, about 10× cheaper than the bilinear write to write‑combined memory.
   Collect the **changed source rows** into ranges.
3. Map each changed source row range to panel rows. Bilinear: panel row y uses
   source rows ⌊s(y)⌋ and ⌊s(y)⌋+1, so widen by one row. Run `drawRows()`
   **only on those panel rows**, and copy the changed rows into the shadow.
4. Fall back to a full draw when the geometry changes (src/dst rects, surface size,
   layer count), on the first frame, and every N seconds as a safety net.
5. Do **not** compare against CA's *other* buffer: with `fb_queue=2`, CA may already
   be drawing the next frame into it.
6. Count changed rows in the 5‑second stats line (`fb_diff` exists but compares
   *output*. That costs as much as drawing and is a diagnostic only).

Optional later: column ranges as well, and an integer 2× path for `fbscale=2`
(750→1440 is 1.92×, not integer, so it is probably not worth it).

**NEON** would speed up blending, but XNU kernel code is built without SIMD (no FP
state saved for kernel threads). Do not do this in the kernel. If needed, move the
scaling into a user-space helper later.

### P5 — CPU clocks (DVFS), only if P1 shows them low

Exynos 990 CPU frequencies are set by the **ACPM** firmware on the APM core, via
an IPC mailbox (`acpm-ipc-channel` in the DTB; Samsung `drivers/soc/samsung/acpm/`,
`cal-if/`, `exynos-acme.c` for CPUfreq). Direct CMU/PLL programming is possible but
undocumented and risky. Research tasks:

- Pull Samsung's `exynos-acme.c`, `acpm_ipc.c`, `fvmap`/`cal-if` from
  `pascua28/android_kernel_samsung_exynos990` (gh API works).
- Find the DVFS IPC message format (`acpm_dvfs`), the channel for CPU clusters, and the
  frequency table (DTB `acme` nodes).
- Implement `redwood_exynos_dvfs.c`: one IPC to set each cluster to a fixed,
  conservative frequency. No governor at first.

### P6 — Userland: make CoreAnimation's job smaller

All of these are settings stock iOS has, written into the system image's preferences:

- **Reduce Transparency** (`com.apple.Accessibility` `EnhancedBackgroundContrastEnabled`,
  also `UIAccessibilityReduceTransparency`). This replaces live blurs with flat fills.
  It is the single most expensive effect for a software renderer.
- **Reduce Motion** (`ReduceMotionEnabled`): cross-fades instead of zooms/parallax.
- Find which `launchd` jobs run (`rc.jobs*`, `mkjobs.py --rest`) and drop those
  useless without hardware (cellular, Wi‑Fi, Bluetooth, camera daemons, etc.).
  Each costs CPU at boot and wakes later.

These need a rebuild of `system.apfs` and a re‑`dd` to `super` (2.3 GB). Batch them.

### P7 — RELEASE kernel

`DEVELOPMENT_ARM64_BCM2837` has assertions and extra checks. Try a RELEASE config once
the port is stable, checking that every `redwood_*` debug path still builds.

## 4. Results log

| Date | Cmdline / change | Lock→home | Driver µs/frame | FPS | Notes |
|---|---|---|---|---|---|
| 24 Oct | baseline `touch_ts=1` (1× A55) | ~45 s | ~80 000 | 2–11 | |
| 24 Oct | `cpus=4` (before the SMP fixes) | ~15 s | ~80 000 | 2–11 | 3 of 4 cores never joined |
| 24 Oct | `cpus=8` + PSCI trampoline + context-id fix | — | ~25 000 | 10–18 | "~50× faster"; 8/8 cores |
| 24 Oct | + `fb_damage` (byte memcmp) | — | 45 000–90 000 | 7–15 | the compare cost more than it saved |
| 24 Oct | `cpumask=0x3f` (no M5s) | — | ~45 000 | 7–15 | "a bit quicker" |
| 24 Oct | + word-wise row compare | — | **7 000–17 000** | 9–16 | **current default** (`y2s.sh` DEFAULT_CMDLINE) |

## 5. What was learnt (24 Oct)

- **Clocks as sboot leaves them** (`perf=1`): A55 **1052 MHz**, A76 **1065 MHz**, Mongoose M5
  **266 MHz**. The "big" cores are the slowest, so they are excluded (`cpumask=0x3f`) until P5.
  P5 (DVFS) is now the biggest remaining CPU lever: ×2 on A55/A76, ×10 on the M5s.
- **Samsung PSCI quirks** (fixed): `CPU_ON` only accepts an entry point below 4 GB, and it
  does **not** pass `context_id` in x0. The loader exports a low trampoline + slot
  (`/chosen/psci-cpu-on-trampoline`, `-slot`), and `psci_smp.c` uses it.
- **MPIDR ≠ DT `reg`** on Exynos 990. The real MPIDRs are A55 `0x81000{0..3}00`, A76
  `0x81000{4,5}00`, and M5 `0x81010000`/`0x81010100`. A55 #1's MPIDR even equals M5 #0's `reg`.
  `start.s` now finds a core's cpu data by context id first. `ml_get_cpu_number_local()` and
  the GIC use the registered phys id.
- `bootcpu=N` (kernel on another core, loader resident on core 0) **does not work here**:
  Samsung's firmware corrupts the resident UEFI when it powers up another cluster. Boot on
  core 0 and let XNU start the rest.
- XNU's generic `memcmp` is byte-wise. Never use it on frame-sized data in the kernel.
- With the display path at ~10 ms, the frame rate is now bound by **CoreAnimation's
  software renderer**, so P5 and P6 are the next steps.
- `lsd` hits the CPU monitor ~700 times in 2 minutes ("denied attempt to escape CPU monitor"):
  it is spinning. That is a P6 item.
- Tearing while scrolling: the frame is written into the buffer DECON scans out, with no vsync.
  It needs a second buffer and a DECON base-address flip, or TE sync (later).
