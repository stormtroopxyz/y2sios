# 04 — GPU: Mali‑G77 MP11 under XNU

## Status (2026‑10‑24 evening): **CoreAnimation draws on the Mali**

The whole stack runs on the phone, and `mali=1` is in `y2s.sh`'s default cmdline:

```
CoreAnimation → EAGL (libredwood-gl, src/eagl.c) → OSMesa → Mesa 25.0.7 panfrost ("Mali-G77 (Panfrost)", GLES 3.1)
  → redwood-drm (libdrm stand-in) → IOKit RedwoodMali (panfrost UAPI 1.3) → bsd/dev/redwood_mali.c → G77
```

| Check (on the phone) | Result |
|---|---|
| `mali_test` (UAPI: CREATE_BO, mmap, SUBMIT, syncobjs, WAIT_BO) | 2001 jobs, **0 faults**, ~64 µs submit + ~104 µs to done |
| `gl_test` triangle at 1440×3200 | right pixels, exact coverage; `GL_RENDERER` = *Mali-G77 (Panfrost)* |
| `gl_test fill` at 1125×2436 | clear 0.33 ms; full-screen textured+blended layer ~1.1 ms; 8 layers 5.5 ms |
| SpringBoard composited by the GPU | "looks much better", no glitches; 24–33 fps; window server waits ~0.1–0.3 ms for the GPU per frame |

### Where the frame time goes now

The GPU is no longer the limit. `MiniIOMobileFramebuffer` still copies and bilinear‑scales each
1125×2436 frame onto the 1440×3200 panel **on the CPU**: 11–20 ms per frame in the driver. The
next performance step is display work (G9):

- the GPU renders the frame at panel size, or blits/scales it into a scanout buffer
- DECON flips between two buffers on vsync. That ends both the CPU copy and the tearing.

### Still missing (not the GL driver)

- **Wallpaper, widgets, some apps:** iOS 16 renders these through **Metal**. The log says
  `metal: no device in this process`. Upstream's Metal work (AIR → NIR, `docs/air.md`) targets
  freedreno/Adreno only. Porting it to panfrost is a separate, large project (G10).
- IRQ‑driven job completion: a scheduler thread polls every 20 µs while jobs are queued.
- G3D DVFS (ACPM `0x0b04000a`). The GPU runs at the boot clock.
- Per‑process GPU address spaces: all clients share AS0.
- Growable heaps: `PANFROST_BO_HEAP` buffers are fully backed up front.

## What was built

| Piece | Where |
|---|---|
| Hardware driver: power cycle + TZPC SMC, reset, G57 quirks, 4‑level page tables, VA allocator, FIFO job scheduler with fault/timeout reset, params | `bsd/dev/redwood_mali.c`, `bsd/sys/redwood_mali.h` (`mali=1`, `mali_selftest=1`, `mali_trace=N`) |
| Panfrost UAPI user client: BOs (write‑combined, IOKit client memory by handle), IOSurface import, SUBMIT/WAIT_BO/syncobjs, show | `iokit/Kernel/RedwoodMali.cpp` |
| Display waits for GPU‑drawn surfaces | `RedwoodGPU.cpp` `MiniIOSurfaceDrawn()` → `RedwoodMaliSurfaceDrawn()` |
| libdrm stand‑in: `panfrost` 1.3, syncobjs, sync files, `drmCloseBufferHandle` | `tools/mesa/redwood-drm/` |
| Mesa patch: libdrm stand‑in for panfrost, OSMesa GPU screen + IOSurface textures, mmap/munmap/import via IOKit, **G77 model row (tilebuffer 32768)** | `tools/mesa/panfrost-darwin.patch` |
| Mesa build on macOS | `tools/mk-mesa.sh` (macOS compiler, meson/ninja in its venv, brew LLVM), `DRIVERS=softpipe,panfrost` |
| GPU userland image on macOS | `userland/build-gpu-y2s.sh` (libOSMesa, libredwood-gl, inject.c/inject_sb.c with the RedwoodMali check, gl_test, mali_test), `rc.jobs.y2s` (no test load), `rc.jobs.mali` (tests) |

## Hardware facts (verified on the phone)

| | |
|---|---|
| Mali registers | `0x18500000`, size `0x5000` |
| IRQs (GIC SPI) | JOB `0x12d`, MMU `0x12e`, GPU `0x12c` (GIC IDs +32) |
| Power domain | PMU `0x15860000`: CONFIGURATION `+0x2200` bit0, STATUS `+0x2204` bit0, OUT `+0x2220` (atomic set alias `|0xc000`) |
| Clocks | CMU_TOP PLL_G3D CON0 `0x1a330100` (bit4 = use PLL), CON1 `0x1a330104`; CMU_G3D `0x18400600` bit4 (G3D_USER mux) |
| TZPC | after power‑on: `smc(0x82000410, 1, 0x18410204, 2)` (returns 0) |
| SYSREG_G3D | `0x18420000`: coherency `+0x400`, AR/AWUSER `+0x420/+0x440`, USER_REG4 `+0x10` (Samsung sets ACE at power‑on; not needed for the first job) |

**Gotcha:** sboot leaves G3D **on but with PLL_CON1 = `0x10000001`** (the value Samsung's *off*
sequence writes). In that state registers, reset and core power‑up all work, but **any GPU
memory transaction hangs**: `AS_COMMAND FLUSH_MEM` never completes. Samsung's
`embedded_g3d_off` → `embedded_g3d_on` (PLL_CON1 = `0x30000003`) → TZPC SMC fixes it.

## Sources

- `Bentlybro/z3s-mainline-linux` (S20 Ultra, same SoC): Panfrost + Mesa working on G77.
  Copies saved in the session's `files/gpu-ref/`. Their kernel delta to Panfrost is tiny:
  `hw_features_g77 = g57`, `hw_issues_g77(_r0p1) = g57`, `GPU_MODEL(g77, 0x9000, …)`.
  Mesa needs a G77 model row with **tilebuffer 32768** (16384 gives `DATA_INVALID_FAULT`).
- Samsung kernel (`pascua28/android_kernel_samsung_exynos990`): `flexpmu_cal_local_exynos9830.h`
  (`embedded_g3d_on/off`), `pmucal_rae.c`, `exynos-el3_mon.c`, `gpexbe_*`.
- Linux `drivers/gpu/drm/panfrost` (regs, MMU, job submission): the register sequences used here.
- Mesa `src/panfrost/genxml/v9.xml`: job header and payload layouts.

## Architecture (mirrors upstream's Adreno path)

```
CoreAnimation ── EAGL (userland/src/eagl.c) ── OSMesa (gallium) ── panfrost (Mesa)
                                                                   │ libdrm calls
                                       tools/mesa/redwood-drm ─────┘ (ioctl low byte = IOKit selector)
                                                                   │ IOConnectCallStructMethod
XNU: RedwoodMali IOKit user client = Linux panfrost DRM UAPI ──────┘
     BOs (contiguous or scattered pages), per-client GPU VA + AS, job slots, IRQs, fences
```

## Steps

| ID | Step | State |
|---|---|---|
| G1 | Read GPU_ID safely (domain check first) | **done** |
| G2 | Power cycle + TZPC SMC + first job (WRITE_VALUE through the MMU) | **done** |
| G3 | Kernel GPU core (power, reset, quirks, page tables, VA, FIFO scheduler, fault/timeout reset) | **done** (polling; IRQs later) |
| G4 | `RedwoodMali` panfrost UAPI user client | **done** |
| G5 | `mali_test` through the UAPI | **done**: 2001 jobs, 0 faults |
| G6 | Mesa panfrost for Darwin + G77 | **done**: `panfrost-darwin.patch`, builds on macOS |
| G7 | `gl_test` triangle, then CoreAnimation on Panfrost | **done** |
| G8 | Performance: IRQs, G3D DVFS, ACE coherency + cached BOs, core power‑down when idle | next |
| G9 | Display: GPU scale/blit to a scanout buffer + DECON flip on vsync (ends the CPU copy and the tearing) | next: biggest frame‑rate win |
| G10 | Metal on panfrost (wallpaper, widgets, Metal apps): upstream's AIR→NIR path, retargeted | large |

## Notes

- XNU pages are 16 KB; GPU pages are 4 KB. One XNU page holds four GPU table levels or four
  GPU pages.
- `kmem_alloc(... KMA_KOBJECT | KMA_ZERO | KMA_DATA)` + `kvtophys` + `dc civac` is the proven
  pattern (also used by redwood_adreno.c). Memory is non‑coherent until G8, so every BO
  handed to the GPU must be cleaned, and invalidated before the CPU reads it.
- `mali=` is **off by default**. With `mali=5` the shader cores stay powered after the test.
