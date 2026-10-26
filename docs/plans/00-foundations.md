# 00 — Foundations: iteration loop, logging, measurement

Everything in the other plans needs many phone runs, and each one must answer a
question. Today a run costs a `dd`, a reboot, and for any log a **panic → Download
Mode → misc reflash → Android → dd** cycle. That is too slow and too manual. This
plan fixes the loop first.

## F1 — Source hygiene: export the kernel work

**Problem.** `hackios-xnu-kernel-main/xnu-src/xnu-8796.121.2` is the working tree.
The durable form is `xnu/redwood.patch` + `xnu/files/` (`xnu/export.sh` writes them,
`xnu/apply.sh` restores them). The y2s work (`redwood_sec_ts.c`, …) was never
exported, so a fresh `apply.sh` would silently lose it.

**Do.** Run `xnu/export.sh` after every kernel change. Check `xnu/files/bsd/dev/redwood_sec_ts.c`
exists afterwards.

**Done when** `diff -r` of a fresh `apply.sh` tree against the working tree shows only build output.

## F2 — One command to build, pack, flash, and read the log

New script `hackios-xnu-kernel-main/y2s.sh`:

| Sub‑command | Does |
|---|---|
| `kernel` | `. common/macos-env.sh; export GIC_V2=1; ./xnu-build.sh -j… -k`, copy to `out/mach.y2s` |
| `payload "<cmdline>"` | `mkpayload.py` with the y2s partition layout + the cmdline, then splice it into `out/y2s-embed.img` at **`0x100870`** (inside the FD's free tail, no checksum, max `0x80000`) |
| `pack` | `out/y2s-recovery.img` = embed@0 + kernel@32 MiB + `root.apfs`@48 MiB |
| `flash` | `adb root`, push, `dd` to `recovery`, verify the kernel region md5, `adb reboot recovery` |
| `log` | `dd` the 2 MiB log at `recovery`+16 MiB, pull it, print it (strip NULs) |
| `cmdline` | print the cmdline currently in `out/y2s-embed.img` |

The cmdline lives in the payload, so a boot‑arg experiment needs **no rebuild**:
`./y2s.sh payload "touch_ts=1 cpus=4" && ./y2s.sh pack && ./y2s.sh flash`.

## F3 — A log without a panic

The loader saves the kernel log to `recovery`+16 MiB whenever the kernel **returns**
(`write_logs()`, loader.c). Handoff §5.5 says the *timer* trigger
`loader_return_after` does not fire on the phone. The **five‑finger hold** in
`redwood_input.c` (`EXIT_HOLD_MS 1500`, calls `loader_return()`) is a different trigger
and now works, because touch works.

**Test:** boot, hold five fingers for 1.5 s, and the loader should print the log and stop.
Then hard‑reset (Vol‑Down + Power), restore `misc`, boot Android, and run `./y2s.sh log`.

- If the loader regains control: this becomes the standard way to end a run.
  B3 adds a power‑button long‑press with the same effect.
- If it does not: find why `loader_return()` fails on this phone. Suspects are the
  return stub's MMU/cache handling on Exynos, and the core the loader parked on (it
  stays on core 0 while the kernel runs there too with `cpus=1`). That also explains
  §5.5.

Second channel, independent of returning: the kernel could write its log ring
**itself** to a reserved RAM area that survives a warm reset, and the loader would dump it
on the next boot. Only build this if F3 fails.

## F4 — Measurement built into the kernel (`redwood_perf.c`)

You can't speed up what you don't measure. A small kernel module, enabled with `perf=1`:

1. **CPU clock per core.** On each core, count `PMCCNTR_EL0` cycles across a fixed
   `CNTVCT_EL0` interval (26 MHz arch timer). Print `cpu N (MPIDR x, A55/A76/M5): ~F MHz`.
   This settles whether sboot leaves the cores slow (deciding P5).
2. **Boot timeline.** `mach_absolute_time` at kernel start, `bsd_init` done,
   launchd exec, first framebuffer swap, then the first swap after SpringBoard is up
   (MiniIOMobileFramebuffer already keeps frame times: `reportFrameTimes`).
3. **Periodic one‑liner** (every 10 s): swaps/s, average and max swap copy time,
   per‑CPU idle %, and memory free. It goes to the log, so a five‑finger exit (F3) shows
   the whole run.

## F5 — Correct the docs

The handoff says "Cortex‑A76, MIDR 0x412FD050". That is an **A55**, the little core.
The plans index records the topology. `handoff.md` §3 should be fixed the same way.

## Iteration protocol (for every experiment)

1. One variable per run (a boot arg or one change).
2. `./y2s.sh payload "…" && ./y2s.sh pack && ./y2s.sh flash`.
3. Note on the phone: time to lock screen and to home screen, how scrolling and
   app launch feel.
4. Five‑finger exit → `./y2s.sh log` → paste the `perf:` lines into the plan's results table.

## F6 — Boot menu in the loader (24 Oct): removed

A volume/power key menu (boot iOS / boot Android with misc restored / power off) was tried and
removed at the user's request. Note: the loader **must** be built with `GIC_V2=1` (`y2s.sh payload`
now does it). Without it, entering the kernel takes a UEFI exception reading `ICC_PMR_EL1`.
