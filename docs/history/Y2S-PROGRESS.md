# y2s (Galaxy S20+ 5G, Exynos 990) — port progress log

## Status — **iOS 16.5 boots on the Galaxy S20+ (Exynos 990)**

| Piece | State |
|---|---|
| Native macOS build (XNU kernel + xnuloader) | **works** (`docs/MACOS.md`) |
| iOS 16.5 fetched; QEMU boots to SpringBoard/lock screen | **works** (baseline) |
| `y2sPkg` (Mu‑Silicium UEFI platform) | **boots on the phone** |
| UEFI on the real phone | **works** — Silicium logo, BDS runs |
| XNU loader (`loader.efi`) on the phone | **works** — EL1, Cortex‑A76, display, platform y2s |
| **XNU kernel on the real phone** | **works** — boots to BSD init, mounts root, panics only without one |
| **iOS userland on the real phone** | **works** — Apple logo, then the iOS home screen (SpringBoard) |
| USB fastboot from the loader | **not available** (firmware has no USB *config* driver, only `UsbfnDwc3Dxe`) |
| GICv2 driver | **works on hardware** (`IIDR 0x0202143b`) |

Verified with no QEMU process running and no USB cable attached: the phone boots iOS
on its own from the recovery partition.

### Boot arrangement (current)
| What | Where |
|---|---|
| UEFI FD + `loader.efi` payload | `recovery` @ 0 (`out/y2s-embed.img`, 2.4 MB) |
| XNU kernel (`mach.y2s`) | `recovery` @ 32 MB |
| `root.apfs` (the `/redwood` disk, md1) | `recovery` @ 48 MB |
| Kernel log (loader writes it) | `recovery` @ 16 MB, 2 MB |
| `system.apfs` (iOS root, md0, 2.32 GB) | `super` @ `0x184B00000` — in `super`'s **unused tail** |

`super` is 8.79 GB; its logical partitions end at sector 12,735,927, so the tail is
free. Writing the root there leaves Android working (Android only needs `super`'s
allocated extents). All of this is written from Android with `dd` — no Download Mode,
no reflash of the phone's other partitions.

`mkpayload.py` entries used:
`--kpart recovery --koff 2000000 --rpart recovery --roff 3000000 --rsize 400000
--rdpart super --rdoff 184b00000 --rdsize 948c0000`

## The three bugs that blocked us (all found and fixed)

### 1. Removing ACPI breaks BDS → black screen → Download Mode
`y2sPkg` was created from `x1sPkg` with the ACPI tables removed ("our loader builds
its own device tree"). That is **wrong**: `BdsDxe`'s dependency expression needs the
ACPI table protocol, so without `AcpiTableDxe` the BDS never dispatches — no console,
no logo, then a reset that the bootloader turns into Download Mode. It looked exactly
like a kernel/image crash and cost many cycles.

**Keep ACPI.** `y2s.dsc`/`y2s.fdf` use `x1s/AcpiTables.inf` from the `Silicium-ACPI`
submodule (same S5E9830 SoC). `UpdateAcpiTables()` itself is a null library.

### 2. Appending the payload to the kernel is rejected by sboot
The prebuilt `Mu-x1s.img` is `shim(112) + FD(2 MB)`, and the ARM64 header's
`image_size` is `FD_SIZE` (2 MB). Appending a `XNUP` payload after the FD makes the
kernel longer than `image_size`; sboot rejects/does not load the tail, so the image
black-screens and falls to Download Mode. **Do not append anything after the FD.**

### 3. `CNTFRQ_EL0` is read-only from EL1
Samsung firmware leaves `CNTFRQ_EL0 = 0`. Writing it to fix that traps
(`synchronous exception`) — it is only writable from EL2/EL3. The loader now uses a
`timer_freq()` helper that falls back to 26 MHz (Exynos 990) for its own timeouts and
for the device tree's `timebase-frequency`.

## Current design

- **Payload lives inside the FD**, at FD offset `0x100000` → physical `0xE8100000`
  (the FV's files end at ~`0x79448`, the FD is 2 MB, so the tail is free and already
  mapped by the existing `UEFI FD` memory descriptor). The stock shim copies the whole
  FD, so no custom shim and no `image_size` mismatch.
- `XnuLoader.h`: `XNU_PAYLOAD_BASE 0xE8100000`, `XNU_PAYLOAD_MAX_SIZE 0x80000`.
- `loader.c`: `PAYLOAD_BASE 0xE8100000`, `PAYLOAD_MAX_SIZE 0x80000`,
  `DOWNLOAD_BASE 0xF0000000` / `DOWNLOAD_MAX_SIZE 0x1000000` (kernel staging, mapped by
  a `XNU Download` descriptor in `y2sPkg`'s `MemoryMapLib`).
- **The kernel is not in the payload**: `mkpayload.py --kpart recovery --koff 2000000`
  tells the loader to read the Mach-O from the `recovery` partition at 32 MB into the
  staging area. The loader then runs it in one-shot mode and **holds the screen**
  (infinite loop) so the panic text can be read.
- Boot image = prebuilt `Mu-x1s.img` shell with our FD spliced in at offset
  `page + 112`. `out/y2s-recovery.img` = that image at 0 + `mach.y2s` at 32 MB.

### Build / pack / flash
```
# UEFI (arm64 Lima VM, reuses the venv)
limactl shell hackios -- bash -lc 'cd /path/to/y2s-workspace/Mu-Silicium && venv-linux/bin/python build_uefi.py -d y2s'
# loader
cd hackios-xnu-kernel-main/xnuloader && GIC_V2=1 ./build.sh
# payload + images: see the python snippet in this repo's history, or pack-y2s.sh
```
Flash `out/y2s-recovery.img` with Heimdall (writes UEFI + kernel in one go).

## Boot chain / recovery facts (hard-won)

- Samsung flashes via **Download Mode** + Odin protocol. Use **samloader‑rs** or
  **Heimdall** (`heimdall-arm64/`, fathonix arm64 fork) on macOS.
- **`adb reboot recovery` sets the boot-recovery flag in `misc` (BCB).** The UEFI does
  not clear it, so every reboot returns to recovery until it is cleared.
- `misc` is 1 MB but the stock `misc.bin` is only ~520 KB and holds Samsung data.
  **Never zero the whole partition** (→ "An error has occurred while updating the
  device software"). Restore the stock `misc.bin`.
- **Force reset on the S20: hold Volume Down + Power ~10–15 s** (Power alone does not
  force off from a hung UEFI screen). Download Mode: Volume Down + Power ~6 s, then
  Volume Up.
- If a flash fails and the phone is already in Download Mode, retries need no key combo.
- adbd drops root after a reboot — `adb root` before `dd`. `stat` is GNU: use `stat -c %s`
  or `wc -c`.

### Recover to Android
```
cd Exynos-iOS
BIN=heimdall-arm64/heimdall-frontend.app/Contents/MacOS/heimdall
"$BIN" flash --MISC samloader-rs/img/misc.bin --RECOVERY y2s-recovery-backup.img
```

## Phone facts
- SM‑G986B (y2s), 12 GB RAM, 1440x3200 display at `0xF1000000`.
- CPU: Cortex‑A76 (`MIDR=0x412FD050`), 8 cores, MPIDRs `{0,1,2,3,4,5,0x100,0x101}`.
- `CNTFRQ_EL0 = 0` (arch timer 26 MHz assumed).
- No `logdump` partition → the UEFI's log-partition channel is a no-op.
- `recovery` partition ≥ 66 MB (stock image is 69 MB); kernel parked at 32 MB.

## Next steps
1. Boot `out/y2s-recovery.img` → read the loader/kernel output on screen.
2. Get XNU past its early boot (GICv2, timer, UART-less console).
3. Then storage (`UfsDxe` exists), touch/keypad, and the userland images.
4. Later: add a USB config driver so the loader can serve fastboot (`UsbfnDwc3Dxe`
   provides `gEfiUsbFunctionIoProtocolGuid`; the loader currently looks for the
   Qualcomm GUID and would need a USBFN-based implementation).
