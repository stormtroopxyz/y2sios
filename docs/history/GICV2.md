# GICv2 support for XNU (Exynos 990 / y2s bring-up)

This is the highest-leverage piece of the y2s port: the Samsung Exynos 990 uses a
**GICv2 (ARM GIC‑400)**, while HackiOS's XNU only had a **GICv3** interrupt driver.
Without interrupts nothing else in the kernel works, so this is done first.

The device tree of the actual phone puts the question beyond doubt
(`android_device_samsung_exynos990-android-12.1/y2s/prebuilt/dtb.img`):

```
/interrupt-controller@10100000 {
    compatible = "arm,cortex-a15-gic", "arm,cortex-a9-gic";
    reg = <0 0x10101000 0x1000>,   // GICD  distributor
          <0 0x10102000 0x1000>,   // GICC  CPU interface (banked)
          <0 0x10104000 0x2000>,   // GICH  hypervisor (unused at EL1)
          <0 0x10106000 0x2000>;   // GICV  virtual CPU interface (unused)
};
```

## What was written

`xnu/files/osfmk/arm64/gic_v2_irq.c` — a drop-in GICv2 driver, guarded by
`GENERIC_GIC_V2_IRQ`. It exports **the same symbols** the GICv3 driver does
(`gic_v3_irq_cpu_init`, `gic_v3_spi_attach`, `gic_v3_irq_signal`, …), so **no caller
changes**: `pexpert/arm/pe_fiq.c`, `osfmk/arm64/psci_smp.c`,
`osfmk/arm64/loader_return.c`, `iokit/Kernel/GenericARMPE.cpp` and
`bsd/dev/redwood_lx.c` all keep working. A board compiles exactly one of
`gic_v3_irq.c` / `gic_v2_irq.c`.

Key GICv2 behaviours implemented:

| GICv3 (redwood/QEMU) | GICv2 (y2s) |
|---|---|
| CPU interface via `ICC_*_EL1` system registers | CPU interface via **MMIO GICC** (`+0x10000` in the node) |
| ack `ICC_IAR1_EL1`, end `ICC_EOIR1_EL1` | ack `GICC_IAR`, end `GICC_EOIR` (EOImode 0, no deactivate) |
| SGI via `ICC_SGI1R_EL1` (affinity) | SGI via **`GICD_SGIR`** (CPU‑interface bitmask) |
| SPI routed with `GICD_IROUTER` | SPI routed with **`GICD_ITARGETSR`** byte |
| per‑CPU SGI/PPI bank in the redistributor | SGI/PPI bank in the distributor (`GICD_ISENABLER0`, banked) |
| `GICD_CTLR` bit 4 ARE + bit 1 EnableGrp1 | non‑secure `GICD_CTLR` bit 0 = EnableGrp1, no ARE |

## Build wiring (already applied to `xnu/redwood.patch`)

The three edits below are **already made** in `xnu/redwood.patch` (the file that
`xnu/apply.sh` runs against Apple's tree), so a y2s build only needs the last step
(`-DGENERIC_GIC_V2_IRQ=1`). They are listed here so they can be reviewed and reverted:

1. **`osfmk/conf/files.arm64`** — the new file is added next to the v3 one:

   ```
   osfmk/arm64/gic_v3_irq.c	optional nos_arm_asm
   osfmk/arm64/gic_v2_irq.c	optional nos_arm_asm
   ```
   (both compile; each is an empty translation unit unless its macro is set, and the
   macros are mutually exclusive per board.)

2. **`pexpert/arm64/pe_fiq.c`** — the `#elif` that starts the controller now accepts
   either macro, so `pe_init_fiq()` calls `gic_v3_irq_cpu_init()` (the name is shared)
   on a GICv2 board too:

   ```c
   #elif defined(GENERIC_GIC_V3_IRQ) || defined(GENERIC_GIC_V2_IRQ)
   ```

3. **`pexpert/pexpert/arm64/BCM2837.h`** — the controller is now a board choice:

   ```c
   #ifndef GENERIC_GIC_V2_IRQ
   #define GENERIC_GIC_V3_IRQ   1   /* redwood, QEMU virt */
   #endif
   #define GENERIC_PSCI_SMP     1
   ```

   A y2s build passes `-DGENERIC_GIC_V2_IRQ=1` (e.g. via `MACHINE_FLAGS_ARM64_BCM2837`
   in `makedefs/MakeInc.def`, keyed off the device profile); the default build is
   unchanged and still GICv3.

4. **`xnuloader/loader.c`** — the `/arm-io/gic` node (not in the patch; do this with M2).
   The GICv2 driver reads `reg = <dist offset, size, cpu interface offset, size>`; the
   GICv3 driver reads `reg = <dist …, redistributor …>`. So the loader must emit the
   right `compatible` and second window per board:

   ```c
   struct platform {
       ...
       uint64_t gic_dist, gic_redist;   /* GICv3: redistributor; GICv2: CPU interface */
   };
   /* in the DT writer, replace the fixed 4-tuple/clone with: */
   #ifdef GENERIC_GIC_V2_IRQ
       dt_prop_str(&w, "compatible", "arm,cortex-a15-gic");
       uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x1000,
                               plat->gic_redist - plat->io_base, 0x1000 };
   #else
       dt_prop_str(&w, "compatible", "arm,gic-v3");
       uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000,
                               plat->gic_redist - plat->io_base, 0x100000 };
   #endif
   ```

   and give `platform_y2s` `gic_dist = 0x10101000`, `gic_redist = 0x10102000`.

**Renaming note.** Exporting `gic_v3_*` names from a GICv2 driver is deliberate, to keep
the first prototype to one file. Once both are known good, rename the shared surface to
`gic_irq_*` across the ~5 callers and split cleanly.

## Test loop 1 — QEMU with a GICv2 (fast, no phone)

QEMU `virt` can present a GICv2 (`-machine virt,gic-version=2`); the GICv2 addresses on
that machine are **GICD `0x08000000`, GICC `0x08010000`**. Add a `platform_qemu_virt_v2`
(or set `GENERIC_GIC_V2_BOARD` and adjust `platform_qemu_virt`'s `gic_redist` to
`0x08010000`) and boot the normal QEMU images. **Exit criterion:** the kernel prints
`gic: v2, distributor …, cpu interface … (IIDR …)`, boots all 4 CPUs via PSCI, and reaches
the lock screen as on GICv3. This validates ack/EOI/timer/SGI/SPI without hardware.

`VERIFY`: the exact `qemu-system-aarch64` invocation that yields GICv2 on `virt` for the
QEMU version the project's `qemu/run-ios.sh` pins.

## Test loop 2 — the phone

With `platform_y2s` in place (M2), the first signs are, in order:
1. the loader's own screen/log (proves the boot chain),
2. the kernel's verbose boot text (`-v`),
3. the `gic: v2, …` line, then PSCI starting the 7 secondaries,
4. `rtclock_intr` ticks — i.e. the virtual timer PPI 27 firing.

## Open questions to close on hardware (in order)

1. **Group/enable bits.** The driver runs non‑secure, so it writes the *non‑secure view* of
   `GICD_CTLR`/`GICC_CTLR` (bit 0 = EnableGrp1). If interrupts never arrive, try also
   setting `GICC_CTLR` bit 1 and check `GICD_TYPER` for the Security Extensions bit. This
   is the first thing to test.
2. **CPU‑interface numbering.** `gic_cpuif_self()` reads an SGI/PPI `GICD_ITARGETSR` and
   takes the set bit; if the SoC returns 0 it falls back to `cpu_number()`. Exynos has 3
   clusters (4×A55 `0x0‑0x3`, 2×A76 `0x4‑0x5`, 2×Mongoose‑M5 `0x100‑0x101`) and the
   GICv2 CPU‑interface wiring is *not* the MPIDR — confirm the discovered numbers are
   `0..7` and in the same order as the cores come up. This matters for targeted SGIs
   (`signalCPU`, stop).
3. **PPI 27** is assumed to be the EL1 virtual timer, as everywhere else; confirm with the
   timer ticking.
4. **Level vs edge** for the first SPI a driver attaches (touch), later.

## Why not GICv3 with the GIC's legacy mode?

The board can't be lied to: there is no GICv3 in the silicon. GICv2's *legacy* CPU
interface (MMIO GICC) is exactly what this driver speaks; the system-register interface
(`ICC_*_EL1`) does not exist on a GIC‑400.

## QEMU GICv2 test results (Apple silicon)

The loop is wired up and runs: `GIC_V2=1` builds the kernel and loader for GICv2,
`qemu/run.sh` uses `-M virt,gic-version=2` (Homebrew edk2 firmware), payload via
`xnuloader/mkpayload.py`. Findings:

1. **A real loader bug, now fixed.** The loader read/wrote the GICv3 CPU-interface
   system registers (`ICC_PMR/IGRPEN1/BPR1_EL1`) *unconditionally* (`loader_save_ctx`,
   `loader_return_stub`, and the "watching only" switch). On a GICv2 those traps
   ("Unknown reason", `elr` at loader `0x10a0`), so the kernel never even started. All
   of it is now `#ifndef GIC_V2` — **needed for the y2s as much as for QEMU**.
2. **The GICv2 kernel boots** under QEMU: past GIC init, all four Cores registered,
   through most of BSD init — it hangs only where it first *needs* an interrupt.
3. **The GICv2 driver is configured correctly**: `GICD_CTLR=1`, `GICC_CTLR=1`,
   `GICD_ISENABLER0` has PPI 27, its priority `0xa0` < `GICC_PMR=0xff`, and
   `GICD_IGROUPR0` shows PPI 27 in **Group 1** — exactly what a real GIC‑400 wants.
4. **The limitation is QEMU's model, not the driver.** QEMU's `virt,gic-version=2`
   reports `GICD_TYPER=0x68` → **SecurityExtn = 0** (a single "Group 0"). With the
   CPU interface enabled, the CPU *does* take an IRQ but `GICC_IAR` acks `1022`
   ("that interrupt belongs to another group"): a non-secure OS cannot get a Group‑1
   IRQ from a GICv2 that has no security extensions. `secure=on` (which would add
   them) does not boot with the plain edk2 firmware.

**Conclusion:** QEMU's GICv2 is a limited bench for this driver — good enough to prove
the register programming and the loader fix, but not a faithful GIC‑400. The definitive
test of Group‑1/IRQ delivery is the real Exynos 990, whose GIC‑400 *does* implement the
security extensions our driver targets.
