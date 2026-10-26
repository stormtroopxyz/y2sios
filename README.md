# y2sios

XNU 8796.121.2 and the iOS 16.5 user space on the Samsung Galaxy S20+ 5G (Exynos 990, SM-G986B, codename `y2s`).

This repository is a source snapshot of a working research port. The current image starts from the phone's `recovery` partition, runs Mu-Silicium UEFI and the `xnuloader` EFI application, enters XNU at EL1, mounts an APFS root, and reaches SpringBoard. Display, GPU, touch, buttons, battery, UFS storage, Wi-Fi, Bluetooth transport and ACPM DVFS are implemented. Cellular, camera, audio output, Metal and Apple services are not.

The port is not a ROM and is not suitable as a daily driver. It is intended for hardware bring-up, driver development and research on recovered or spare hardware.

## Status

Current as of 26 October 2026.

| Area | State | Notes |
|---|---|---|
| Boot to SpringBoard | Working | Standalone boot from `recovery`; no Android or host computer required |
| Display | Working | 1440x3200, tear control, hardware scaler, brightness, display power |
| Touch | Working | Samsung `sec_ts`, multitouch, loader escape gesture |
| Buttons and battery | Working | GPIO keys, MAX77705 gauge and charger state |
| GPU | Working | Mali-G77 MP11 via a Panfrost UAPI driver and Mesa 25.0.7; OpenGL ES for CoreAnimation |
| Wi-Fi | Working | BCM4375 over PCIe, firmware 18.41.117, WPA2, DHCP, IPv4 and IPv6, HTTPS |
| Storage | Working | Native UFS driver, read-only root from `super`, HFS+ `/private/var` on `userdata` |
| Bluetooth | Partial | HCI transport, patchram and kernel control; profiles and pairing untested |
| Audio | Not working | ABOX and CS35L41 configured; RDMA3 consumes zero frames, output silent |
| Metal | Parked | Pipelines build; CoreAnimation function constants arrive as zero |
| 120 Hz | Not implemented | Requires an FHD mode set at 1080x2400 |
| Cellular | Not planned | Shannon 5123 has no open firmware interface; iOS baseband stack targets other vendors |
| Camera | Not implemented | No Exynos ISP or camera pipeline |
| Haptics, sensors, GNSS | Not implemented | No drivers |
| Apple services | Not possible | No Secure Enclave; FairPlay and Apple ID sign-in cannot work |
| Sleep | Not implemented | Display power only; no SoC suspend or wake policy |
| Charging control | Not implemented | MAX77705 runs on bootloader defaults; driver is read-only |
| M5 cores | Disabled | All eight boot; Safari WebContent crashes with the M5 cluster enabled |
| WPA3 | Implemented, untested | Firmware supports SAE; `wifid` capability bit not identified |

## Target hardware

| | |
|---|---|
| Model | SM-G986B, `y2s`, Galaxy S20+ 5G |
| SoC | Samsung Exynos 990, S5E9830 |
| CPU | 4x Cortex-A55 (`ananke`), 2x Cortex-A76 (`enyo`), 2x Mongoose M5 (`lion`) |
| GPU | Mali-G77 MP11, Valhall |
| RAM | 12 GB |
| Display | 6.7 inch, 1440x3200, Samsung `s6e3hab`, MIPI command mode |
| Touch | Samsung LSI S6SY79A, `sec_ts` protocol, I2C address 0x48 |
| Interrupt controller | ARM GIC-400, GICv2, distributor at 0x10101000 |
| Storage | UFS 3.0, controller at 0x13100000 |
| PMICs | S2MPS19, S2MPS22, S2DOS05, S2MPB02 |
| Fuel gauge and charger | MAX77705 |
| Wi-Fi and Bluetooth | Broadcom BCM4375, PCIe and UART |
| Audio | Exynos ABOX and two Cirrus CS35L41 amplifiers |
| Bootloader | Samsung `sboot`, unlocked |

Hardware addresses and pin assignments are listed in [docs/hardware.md](docs/hardware.md). The decompiled stock device trees in [reference/dts](reference/dts) are the primary source for those values.

## System architecture

```
sboot
  -> boot or recovery partition
       Android boot image
         BootShim
           copies the 2 MiB UEFI firmware volume to 0xE8000000
         Mu-Silicium UEFI, y2sPkg
           BDS finds no boot option
           calls XnuLoaderMain()
             starts loader.efi from the XNUP payload at 0xE8100000
         xnuloader
           reads the kernel and images
           builds a device tree and boot_args
           exits boot services, disables the MMU, jumps to XNU
  -> XNU
       GICv2, arch timer, PSCI SMP, APFS, HFS+
       launchd, backboardd, SpringBoard
```

The four source layers are:

| Layer | Source | Scope |
|---|---|---|
| UEFI platform | `uefi/` | Mu-Silicium `y2sPkg`, loader hook, platform memory map |
| Loader | `kernel/xnuloader/` | XNUP payload, device tree, boot args, boot menu, USB flashing |
| Kernel | `kernel/xnu/` | XNU overlay, GICv2, drivers, IOKit services |
| User space | `userland/` | init, injected libraries, launchd jobs, tests, GPU stack |

### Partition layout

The `recovery` partition is used as the port's disk. The stock partition is 65.8 MiB.

| Offset | Size | Contents |
|---|---|---|
| 0 | 2.4 MB | Android boot image: BootShim, UEFI firmware volume, XNUP payload |
| 16 MiB | 2 MiB | Kernel log, written by the loader on return or panic |
| 32 MiB | 16 MiB | `mach.y2s`, the XNU kernel |
| 48 MiB | 4 MiB | `root.apfs`, the small `/redwood` volume |
| `super` + 0x184B00000 | 2.32 GiB | `system.apfs`, iOS root in the unused tail of `super` |

`super` is 8.79 GB. Its logical partitions end at sector 12,735,927, so the tail does not overlap Android data. The kernel either loads the system image into RAM or mounts it from `super` through its UFS driver; the current configuration uses the second path and saves about 2.3 GB of RAM.

Two boot-chain constraints are fixed:

- The XNUP payload is placed inside the UEFI firmware volume at offset `0x100000`, physical address `0xE8100000`. The stock boot image header records `image_size` as 2 MiB, so appending data after the firmware volume causes `sboot` to reject the image or drop the tail.
- `y2sPkg` keeps the ACPI tables from `x1s/AcpiTables.inf`. `BdsDxe` depends on the ACPI table protocol; removing `AcpiTableDxe` prevents BDS from dispatching and produces a blank screen followed by Download Mode. The tables are otherwise inert because `UpdateAcpiTables()` is a null library.

### Kernel configuration

| | |
|---|---|
| Source | Apple `xnu-8796.121.2` |
| Darwin version | 22.5.0 |
| Build configuration | `BCM2837`, used as a generic ARM64 base |
| Interrupt build flag | `GENERIC_GIC_V2_IRQ`, set by exporting `GIC_V2=1` |
| Output | `mach.development.bcm2837`, copied to `out/mach.y2s` |
| Size | About 16.63 MB in a 16.78 MB slot |

`kernel/xnu/` is an overlay, not a complete XNU tree. `apply.sh` clones Apple's source and applies `redwood.patch` plus the files in `files/`. `export.sh` regenerates both from a working tree.

## Kernel implementation

### Interrupt controller

The Exynos 990 uses a GIC-400 (GICv2). The inherited HackiOS kernel had a GICv3 driver, which accesses `ICC_*_EL1` system registers that are undefined on this SoC. The first access faults immediately after `pe_arm_init_debug`, without a usable panic.

`kernel/xnu/files/osfmk/arm64/gic_v2_irq.c` implements GICv2 behind the same exported symbols as the GICv3 driver, so callers are unchanged. A board compiles one of the two drivers.

| GICv3 | GICv2 on y2s |
|---|---|
| CPU interface via `ICC_*_EL1` | CPU interface via MMIO at `GICC`, base + 0x10000 |
| `ICC_IAR1_EL1` / `ICC_EOIR1_EL1` | `GICC_IAR` / `GICC_EOIR` |
| SGIs via `ICC_SGI1R_EL1` | SGIs via `GICD_SGIR` |
| SPIs via `GICD_IROUTER` | SPIs via `GICD_ITARGETSR` |
| Per-CPU bank in a redistributor | SGI and PPI bank in the distributor |

The driver reports `gic: v2`; hardware reports `IIDR 0x0202143b`. `y2s.sh kernel` checks for the `gic: v2` string before packing, because a GICv3 kernel produces a silent boot failure.

QEMU's GICv2 model is insufficient for validating the driver: it has no security extensions, reports `GICD_TYPER=0x68`, and acknowledges interrupts as `1022`. Validation is performed on the phone.

### Timer and SMP

`CNTFRQ_EL0` reads zero. Writing it from EL1 raises an illegal instruction exception because the register is writable only from EL2 or EL3. The loader does not write it. `timer_freq()` returns `CNTFRQ` when valid and 26 MHz otherwise, and the device tree publishes 26 MHz as `timebase-frequency`.

PSCI is used over SMC. The MPIDR table is `{0, 1, 2, 3, 4, 5, 0x100, 0x101}`. The loader writes explicit CPU nodes, and the kernel scheduler marks the A55 cores as slow. The default `cpumask=0x3f` uses the four A55 cores and the two A76 cores. The M5 cluster is excluded because a run with `cpumask=0xff` crashed Safari's WebContent process on every page; the cause is unresolved.

The boot core is MPIDR 0, a Cortex-A55 (`MIDR 0x412FD050`).

### Boot arguments

The command line is stored in the XNUP payload and can be changed without rebuilding the kernel.

| Argument | Effect |
|---|---|
| `cpumask=0x3f` | Four A55 and two A76 cores |
| `cpus=N` | Number of cores; do not combine with `cpumask` |
| `touch_ts=1` | Probe and poll touch; `2` probe only, `4` reset the bus |
| `bms=1` | MAX77705 battery and charger |
| `mali=1` | Start the Mali and publish the GPU service |
| `mali_irq=1` | Use the GPU job interrupt |
| `mali_cached=1` | Cached GPU memory |
| `fb_te=1` | Tear-controlled copy path |
| `fb_scaler=1` | Zero-copy display scaler |
| `fb_bl=1` | Brightness and display power through DSIM |
| `fb_threads=N` | Helper threads for the copy path |
| `dvfs=2` | ACPM DVFS |
| `perf=1` | Per-core clocks and boot timing |
| `wifi_pcie=1..4` | PCIe look, link, DMA grants, full Wi-Fi |
| `wifi_pcie_wait=N` | Seconds before PCIe initialisation |
| `bt=1` | Bluetooth UART and HCI bring-up |
| `snd=1` | ABOX and amplifier bring-up |
| `ufs=2` | Kernel UFS driver |
| `ufs_native=1` | Native command list |
| `ufs_root=super:OFFSET:SIZE` | Mount iOS root from `super` |
| `ufs_write=1` | Allow writes in the userdata window |
| `var_hfs=1` | HFS+ on `userdata` as `/private/var` |
| `debug=0x8` | XNU debug flags |

A full working command line is in [Build and pack](#build-and-pack).

## Subsystems

### Display

The panel is a Samsung `s6e3hab`, 1440x3200, DSC 3:1, MIPI command mode with frame memory. DECON_F at `0x19050000` sends a frame over DSI on each TE pulse. iOS identifies the device as an iPhone X and renders at 1125x2436.

Three display paths are implemented in `kernel/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`:

- **CPU copy with tear control (`fb_te=1`).** Frames are drawn into a cached back buffer. On present, the driver masks the DECON hardware trigger (`HW_TRIG_MASK_DECON`, bit 4 of `0x70`), waits for DECON to become idle, clears the pending frame-start flag, copies changed rows on helper threads, checks for a race, and unmasks. A masked trigger produced zero frames in 40 ms, confirming that the mask holds the panel.
- **Zero-copy hardware scaling (`fb_scaler=1`).** IDMA channel 5 and DPP channel 5 are configured, and window 5 is pointed at channel 5 instead of S-Boot's channel 0. A Keys bicubic filter scales 1125x2436 to 1440x3200 in the display controller. The log reports frames taken by the controller, for example 46 controller frames and one processor frame in a five-second interval.
- **CPU fallback.** Frames that cannot use the scaler are copied and scaled by the CPU, and the window returns to S-Boot's channel.

The scaler uses physically contiguous IOSurfaces below 4 GB. The loader reserves a 64 MB display pool because XNU does not manage memory below 4 GB on this platform. Surfaces are mapped write-combined. Swap tickets make `present()` wait until the frame has been latched, and the vsync is scheduled 0.3 ms after the last latch.

Brightness uses AOR (the fraction of each frame for which the OLED emitters are held off) rather than DBV alone. The panel sequence is:

```
F0 5A 5A          level-2 key
B1 hi lo          AOR
6A hi lo 01       DBV
53 00             HBM off
F7 0F             latch
F0 A5 A5          key off
```

AOR is linear in nits between the measured endpoints:

```
AOR = 0x0C27 - (0x0C27 - 0x01E4) * nits / 420
```

`0x01E4` is full brightness and `0x0C27` is the darkest value. The driver publishes an `AppleARMBacklight` service with `backlight-control = true`, which CoreBrightness requires before it creates a display module. Display off and on use DCS `28` and `29` through DSIM0 at `0x190C0000`.

Not implemented: 120 Hz. The panel requires an FHD mode set at 1080x2400; the DCS sequence and DECON/DSC values are documented in [docs/plans/07-display.md](docs/plans/07-display.md).

### GPU

The GPU stack is:

```
CoreAnimation
  -> EAGL (libredwood-gl, userland/src/eagl.c)
  -> OSMesa / Mesa 25.0.7 panfrost
  -> libdrm stand-in (tools/mesa/redwood-drm)
  -> IOKit RedwoodMali (Panfrost UAPI 1.3)
  -> kernel/xnu/files/bsd/dev/redwood_mali.c
  -> Mali-G77 MP11
```

The kernel driver performs the power cycle and the DTZPC restore (`smc(0x82000410, 1, 0x18410204, 2)`), resets the GPU, applies G57 errata, builds four-level page tables, allocates virtual address space, and runs a FIFO job scheduler with fault and timeout recovery. The Mesa patch adds a G77 model row with a 32768-byte tilebuffer; 16384 bytes produces `DATA_INVALID_FAULT`.

Measured on hardware:

| Test | Result |
|---|---|
| `mali_test` UAPI job loop | 2001 jobs, zero faults |
| `gl_test` triangle at 1440x3200 | Correct coverage |
| Full-screen textured and blended layer | About 1.1 ms at 1125x2436 |
| Eight layers | About 5.5 ms |
| SpringBoard compositing | GPU path active, job interrupt at GIC 333 |
| GPU clock | 260 MHz at boot, 702 MHz after ACPM DVFS |

Metal is implemented but parked. `userland/src/redwoodmetal.m` and `tools/mesa/air` translate AIR to NIR and run it on Panfrost. Pipelines build and SpringBoard runs, but CoreAnimation's `fixed_frag_lph_cph` uber-shader is specialised by 58 function constants, and the translator supplies zero for each because the expected setters are not called. The log reports `0 values (tied)`. This causes square corners, missing fills, missing masked content and a Settings crash. The next step is to read `MTLFunctionConstantValuesInternal` or decode `serializedConstantDataForFunction:dataSize:errorMessage:`.

### Input, buttons and power

Touch is a Samsung S6SY79A using the `sec_ts` y79a protocol: 16-byte events, command `0x60` for one event, `0x61` for additional events, `0x62` to clear the stack, and `0x10` for sense on. Coordinates are 12-bit and scaled to 0 to 65535 before injection.

The controller is on HSI2C_15, but the driver bit-bangs I2C on `gpp5-6` (SCL) and `gpp5-7` (SDA) at about 125 kHz. The HSI2C controller's IP clock is gated by a `cmu_peric1` block whose base address is absent from this phone's downstream device tree, and the controller registers remain at reset values. The bit-bang path does not require the CMU clock.

`tsp_io` and `tsp_avdd` are LDO2 and LDO3 of an S2DOS05 at I2C 0x60 on `hsi2c_4` (`gpp1-0` and `gpp1-1`). The driver enables LDO2, waits 1 ms, enables LDO3, and waits 100 ms. Before the rails are enabled, the touch controller clamps the bus low, a ghost address appears to acknowledge, and reads return zero.

The attention line is `gpa1-0`. The driver polls it and accesses the bus only when it is low. Five fingers held for 1.5 seconds, or volume down plus power for about 3 seconds, returns to the loader.

Keys are plain active-low GPIOs on the ALIVE pinctrl at `0x15850000`: volume up `gpa0-3`, volume down `gpa0-4`, power `gpa2-4`.

The battery driver reads a MAX77705 fuel gauge at 0x36 and charger at 0x69 every 5 seconds over a shared bit-banged bus. It reports through `redwood_battery_report()` to the `RedwoodBattery` IOPMPowerSource service. It is read-only; charger configuration is left as `sboot` set it.

### Storage

`kernel/xnu/files/bsd/dev/redwood_ufshc.c` implements the Exynos UFS host controller at `0x13100000`. It takes over the controller initialised by UEFI's `UfsDxe` and builds its own transfer list. The Exynos quirks handled by the driver are:

- Response and PRDT offsets and lengths are in bytes.
- PRD entries are at most 4 KB.
- `UTRLCLR` is cleared by writing ones.
- Every slot's bit in `UTRL_NEXUS_TYPE` must indicate a SCSI command. The vendor register is at `0x1140`.
- The doorbell bit is not taken back while `IS.UTRCS` is set. The driver treats a slot as complete when the doorbell bit is clear or the OCS is no longer the invalid value, then clears `UTRCS` and the slot's `UTRLCNR` bit, and uses `UTRLCLR` only if needed. In the md5 validation run, 2715 slots were retired by the controller and 7843 by the acknowledgement; none were stuck.

The kernel exposes 33 partitions as `/dev/ufs_NAME` and `/dev/rufs_NAME`. Reads are whitelisted. Writes are restricted to `userdata` through the write window in `redwood_ufs.c`. The iOS root is a read-only window of `super` at `0x184B00000`.

`/private/var` is a case-sensitive journaled HFS+ file system on `userdata`, created on first boot from a 60 KB kit and mounted by init. The write path reads back and compares every 4 MB, and close sends `SYNCHRONIZE CACHE`. HFS+ is linked into the kernel rather than loaded as a kext, which contributes to the kernel size pressure.

### Wi-Fi

The BCM4375 is a PCIe endpoint behind the Exynos DesignWare host controller with Samsung's host-v0 wrapper (`kernel/xnu/files/bsd/dev/redwood_exynos_pcie.c`).

| Resource | Address or pin |
|---|---|
| ELBI | `0x133B0000` |
| PHY | `0x133E0000` |
| PCS | `0x133D0000` |
| DBI | `0x133F0000` |
| Configuration window | `0x11FFE000` |
| Memory window | `0x11000000` |
| PMU PHY isolation | `0x15860718` |
| CMU_HSI1 | `0x13000000` |
| `WLAN_REG_ON` | `gpb0-4` |
| `PERST#` | `gpf0-1` |
| `CLKREQ` | `gpf0-0` |

Bring-up is controlled by `wifi_pcie`:

| Value | Action |
|---|---|
| 1 | Read-only inspection |
| 2 | REG_ON, clocks, PHY isolation, PHY tables, root port, PERST#, LTSSM |
| 3 | DMA grants |
| 4 | Full driver, firmware, station and `en0` |

The default wait is 20 seconds so that a stalled PCIe bring-up leaves a usable phone. Working runs use `wifi_pcie_wait=3` so `en0` exists before `wifid` starts.

The DMA pool is 32 MB below 4 GB, reserved by the loader and published in `/chosen` as `wifi-dma-base` and `wifi-dma-size`. Samsung's hypervisor grants each 64 KB granule with `hvc(0xc6000101, (1 << 16) | 24, granule, 0x10000, 3)`. The log reports 512 granted and none refused. XNU's memory is above 4 GB, which the BCM4375 cannot address directly.

The driver reads the chipcommon ID, walks the EROM, locates the PCIe2 core and mailbox registers, and loads firmware 18.41.117 from `/redwood/fw/bcm4375-sta.bin` with the SEMCO NVRAM. It starts the ARM, waits for the shared-area address, sets `host_cap 0x00821407`, posts the common rings, and brings up msgbuf. It then loads CLM, sets `mpc 0`, enables the event mask, calls `WLC_UP`, and runs an escan.

The firmware has no supplicant: `sup_wpa` returns -23 (`BCME_UNSUPPORTED`). `kernel/xnu/files/bsd/dev/lx_wpa.c` performs the WPA2 four-way handshake in the kernel, answers messages 1 and 3, sends message 4 before installing the pairwise key, and installs the group key through `wsec_key`.

Measured results:

- Endpoint ID `14e4:4475`.
- Firmware banner `wl0: Jul 19 2022 10:52:21 version 18.41.117 (B1 Network/rsdb)`.
- CLM `9.9.5_Y2LSI`.
- Scan results on 2.4 and 5 GHz.
- WPA2 join at 5180 MHz with a completed handshake.
- Safari loaded `google.com` and `apple.com`; 44 MB received and 1.7 MB sent with no drops.
- IPv4 DHCP and IPv6 addressing both work.

WPA3 is implemented but untested. `wifid` rejects a WPA3-only network unless the driver advertises a capability bit that has not yet been identified in IO80211.

### Bluetooth

The Bluetooth half of the BCM4375 is on USI16 at `0x10650000`. `kernel/xnu/files/bsd/dev/redwood_exynos_bt.c` programs the UART clock to 200 MHz, enables the gates, brings the chip up at 115200 baud, issues an HCI reset, reads the local version, and loads `bcm4375B1-semco.hcd` as patchram. Patchram writes 1260 records in about 1.9 seconds. The chip then reports HCI revision `00ad`, and the driver switches to 3 Mbaud.

The iOS transport model was derived by disassembly. `bluetoothd` starts only when a non-zero BD address exists and one of the following is present: a kernel control named `com.apple.uart.bluetooth`, a device node, or an IOService named `bluetooth` or `marconi-bt`. The transport is selected by `BT_HCI_TRANSPORT`. iOS opens a kernel control socket (`PF_SYSTEM`, `SYSPROTO_CONTROL`), configures termios through socket options, and then uses plain `read` and `write`. The driver publishes both the IOService and the control. `BlueTool` is replaced by a small stand-in because Apple's version carries an iPhone X patchram and would load the wrong firmware.

`bluetoothd` connects to `com.apple.uart.bluetooth`, and HCI command complete and status events are returned. `marconi-bt` still logs "Unable to match bluetooth service in device tree" and "Not a valid MAC address (all 00s)", so service discovery is incomplete. Pairing, A2DP, HFP and LE are untested.

The BD address is currently a placeholder; the real address is stored in EFS and has not been read yet.

### Audio

The hardware path is implemented in `kernel/xnu/files/bsd/dev/redwood_exynos_sound.c` and skips the Calliope DSP:

```
RDMA3 -> SPUS chain 3 -> SIFS0 -> UAIF2 -> 2x CS35L41
```

The AP programs the ABOX directly: `pd-aud` power, DTZPC restore (`smc(0x82000410, 1, 0x18e10204, 2)`), pad retention, PLL_AUD0 at 1179.648 MHz, AUDIF at 73.728 MHz, UAIF2 BCLK at 3.072 MHz, and a 32 KB RDMA ring in the loader's DMA pool. Both CS35L41 amplifiers answer at 0x40 and 0x41 and are configured as Cirrus's driver does: reset, OTP boot, trim unpack, slave DSP_A, 3.072 MHz SCLK, ASPRX1 to DAC. The left amplifier uses slot 0 and the right uses slot 1.

The failure is in the DMA path:

```
snd: after 250437 us the DMA read 0 frames, the clock says 12020: following the clock
snd: 162304 frames from iOS, 200704 fed, 18618662 silent, 1 underruns; DMA at 0
```

RDMA3 does not advance. Candidates are the route registers, SPUS and SIFS configuration, the UAIF2 frame clock, and the amplifier ASPRX path. The `snd:` log lines contain the register readbacks needed to diagnose it. The plan is in [docs/plans/08-audio.md](docs/plans/08-audio.md).

### Clocks and power

`kernel/xnu/files/bsd/dev/redwood_exynos_acpm.c` communicates with Samsung's ACPM firmware in SRAM at `+0x7000`. The driver enumerates the shared-memory descriptors at boot (9 plugins, 11 channels) and sends DVFS requests through the mailbox.

Measured:

```
dvfs: PLL_G3D 260 -> 702 MHz: ACPM DVFS works
dvfs: domain 2 to 1742000 kHz: answered
dvfs: domain 3 to 2210000 kHz: answered
```

The A55 cluster reaches 1.742 GHz, the A76 cluster 2.21 GHz, and the GPU 702 MHz. Temperatures under light load are around 60 C. The M5 cluster is a separate domain and remains unused. There is no thermal throttling policy beyond the firmware's own behaviour; `redwood_perf.c` and `thermald.c` provide measurement only.

### Standalone operation

The loader presents a boot menu drawn with XNU's 8x16 console font: **Boot iOS**, **Log access (USB)**, **Download Mode**, **Reboot** and **Power off**. It appears at power-on and after iOS returns, and boots iOS automatically after 3 seconds if no key is pressed.

Log access starts a USB device stack on `EFI_USBFN_IO_PROTOCOL` and a fastboot server. `y2s.sh ulog`, `uflash`, `uboot` and `hflash` use it. `uflash` writes the image in two pieces around the 16 MiB log region so the previous log survives. The USB implementation handles `CLEAR_FEATURE(ENDPOINT_HALT)` by resetting the DWC3 data sequence, which is required for repeated sessions with macOS `fastboot`.

While iOS runs, `com.redwood.logd` serves the kernel log ring and message buffer over HTTP on port 8080, advertised by Bonjour as `HackiOS`. `y2s.sh wlog` and `wtail` use it.

## Known limitations

| Limitation | Cause | Next step |
|---|---|---|
| No audio output | RDMA3 consumes zero frames | Inspect SPUS/SIFS/UAIF2 and amplifier ASPRX |
| Metal rendering incorrect | 58 function constants supplied as zero | Read `MTLFunctionConstantValuesInternal` or decode serialized constants |
| M5 cluster unstable | Unknown; WebContent crashes with `cpumask=0xff` | Compare per-core registers and cache/voltage configuration |
| No 120 Hz | Requires FHD mode set | Implement the documented DCS and DECON mode change |
| No camera | No Exynos ISP or camera pipeline | New subsystem; upstream driver is Qualcomm-specific |
| No cellular | Shannon 5123 firmware interface is undocumented; iOS baseband stack targets other vendors | Not planned |
| No Apple services | No Secure Enclave or FairPlay | Hardware limitation |
| No sleep | No SoC suspend or wake-source policy | Design and implement platform sleep |
| No charging control | MAX77705 is read-only in this port | Add a reviewed charger policy |
| No haptics, sensors or GNSS | Drivers not written | CS40L25, grip sensor and BCM4773 bring-up |
| Bluetooth profiles untested | HCI and control transport only | Pairing and profile work |
| WPA3 untested | `wifid` capability bit unknown | Identify the IO80211 capability |
| Kernel size pressure | 16.63 MB of 16.78 MB used | Move `root.apfs` or reduce linked subsystems |
| WPA2 supplicant in kernel | Samsung firmware lacks `idsup` | Keep in kernel or move to user space |

## Build

The repository is a snapshot and is applied over the upstream HackiOS trees. It is not a standalone clone. The full procedure, including the UEFI build in an arm64 Linux VM, is in [docs/building.md](docs/building.md).

### Requirements

- Apple silicon Mac with Xcode Command Line Tools.
- Homebrew `coreutils`, `gnu-tar`, `gnu-sed`, `findutils`, `bison`, `qemu`, `sevenzip`, `lz4`, `dtc`.
- Theos with `iPhoneOS16.5.sdk` at `~/theos/sdks/iPhoneOS16.5.sdk`.
- Python 3, `clang`, `lld`.
- arm64 Linux host or VM for Mu-Silicium; Mu-Basetools has no macOS host support.
- Unlocked SM-G986B, `adb`, `fastboot`, Heimdall.
- iOS 16.5 IPSW, downloaded by `ios165/fetch.sh` from Apple's CDN.
- The phone's own firmware blobs, extracted from the stock AP tar. See [reference/firmware-notes.md](reference/firmware-notes.md).

### Kernel and loader

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git
cd hackios-xnu-kernel
common/setup.sh

SNAP=/path/to/y2sios
rsync -a "$SNAP/kernel/xnu/" xnu/
rsync -a "$SNAP/kernel/xnuloader/" xnuloader/
rsync -a "$SNAP/kernel/common/" common/
rsync -a "$SNAP/kernel/tools/" tools/
rsync -a "$SNAP/userland/src/" userland/src/
cp "$SNAP/kernel/y2s.sh" .
cp "$SNAP/userland/build-gpu-y2s.sh" "$SNAP/userland/build.sh" \
   "$SNAP/userland/mkjobs.py" "$SNAP/userland/mkeagl.py" userland/
cp "$SNAP/userland"/rc.* userland/
cp "$SNAP/userland"/*.plist userland/
cp "$SNAP/userland/hfs-kit.bin" userland/
rsync -a "$SNAP/userland/libmini/" userland/libmini/

xnu/apply.sh
xnu-deps/setup.sh
export GIC_V2=1
./xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k
```

`./y2s.sh kernel` wraps the build, checks for `gic: v2`, copies the kernel to `out/mach.y2s`, and exports the overlay. The build exits non-zero because of XNU's symbolset export check; the kernel is linked before that step.

### UEFI

```sh
git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git
cd Mu-Silicium
git apply /path/to/y2sios/uefi/y2s.patch

cp -a /path/to/y2sios/uefi/Platforms/Samsung/y2sPkg Platforms/Samsung/
cp /path/to/y2sios/uefi/Resources/Configs/y2s.toml Resources/Configs/
cp /path/to/y2sios/uefi/Resources/DTBs/y2s.dtb Resources/DTBs/
cp /path/to/y2sios/uefi/BootManagerLib/* \
   Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/
```

Build in the Linux VM:

```sh
python3 build_uefi.py -d y2s
```

Output: `Build/y2sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd`, 2 MiB.

### Userland

```sh
cd userland
../ios165/fetch.sh
RC_JOBS=rc.jobs.y2s ./build-gpu-y2s.sh
```

`build-gpu-y2s.sh` builds Mesa with panfrost, the EAGL library, the Metal libraries and tests, then calls `build.sh` with `GL=1`. `build.sh` produces `root.apfs` and the 2.3 GB `system.apfs`. Apple files and the system image are not committed.

### Build and pack

From the kernel tree:

```sh
./y2s.sh payload "touch_ts=1 bms=1 mali=1 cpumask=0x3f fb_threads=6 \
  fb_te=1 fb_bl=1 fb_scaler=1 dvfs=2 perf=1 mali_irq=1 mali_cached=1 \
  wifi_pcie=4 wifi_pcie_wait=3 ufs=2 ufs_native=1 \
  ufs_root=super:184b00000:948c0000 ufs_write=1 var_hfs=1 bt=1 snd=1"
./y2s.sh pack
```

`payload` builds the loader and XNUP container and records the size of `root.apfs`. `pack` writes `out/y2s-recovery.img` as:

```
embed @ 0 + kernel @ 32 MiB + root.apfs @ 48 MiB
```

Run `payload` again whenever `root.apfs` changes size. The loader reads the recorded size; a mismatch causes an APFS mount failure and a halt on the UEFI screen.

### Flash

| Path | Command |
|---|---|
| From Android | `./y2s.sh flash` |
| From the loader menu | `./y2s.sh uflash` |
| From Download Mode | `./y2s.sh hflash` |
| Install the boot image into `boot` | `./y2s.sh standalone` |

`flash` uses `adb root`, writes the image to `recovery`, compares the md5 by reading the partition back, and reboots into it. `uflash` writes over USB in two pieces around the log region. `hflash` uses Heimdall.

## Recovery and safety

- Force reset: Volume Down and Power for 10 to 15 seconds.
- Download Mode: Volume Down and Power for about 6 seconds, then Volume Up.
- `adb reboot recovery` sets the boot-recovery flag in `misc`. The UEFI does not clear it, so subsequent reboots return to the UEFI until the stock `misc` file is restored.
- Never zero the whole `misc` partition. Restore the stock file.
- After `standalone`, restore Android's boot image with `heimdall flash --BOOT backup-y2s/boot.img`.
- Do not write `sdb`, `sdc`, `efs`, `sec_efs`, `param`, `up_param`, `vbmeta*`, `keydata`, `keyrefuge`, `steady` or `persistent`.
- `by-name/sda` is the whole disk. Scripts over `by-name/*` must exclude `sda` to `sde`.
- Keep partition backups and a stock AP tar.

The loader's `oem write` accepts `boot`, `recovery`, `super` above `0x184B00000`, and `userdata` only.

## Debugging

The kernel log is the primary instrument. It is available through three paths:

1. **Loader return.** Volume Down plus Power for 3 seconds, or five fingers for 1.5 seconds, returns to the loader, which writes the log to `recovery`+16 MiB and displays it.
2. **USB.** Select Log access in the menu and run `./y2s.sh ulog`.
3. **Wi-Fi.** While iOS runs, `./y2s.sh wlog` fetches the log ring and message buffer; `wtail` follows it.

`y2s.sh log` pulls the saved log from the recovery partition over adb and strips NUL bytes. The loader's log can contain NUL data; earlier tooling stopped at the first NUL and truncated the result.

Useful hardware techniques:

- **Write-back test.** Write a value to a register and read it back. A value that does not stick indicates a gated clock or unpowered block.
- **Ghost I2C address.** Access an unassigned address. An acknowledgement indicates a stuck-low data line, not a working bus.
- **Deliberate panic.** A panic message is the last entry in the log and survives to storage. Include register values in the message.
- **Instrument checks.** [docs/history/checks.md](docs/history/checks.md) documents measurement failures from this port, including skipped cases counted as successes and stale build artefacts.

## Repository layout

```
y2sios/
  README.md
  LICENSE
  NOTICE.md
  docs/
    history/                 port log, handoff, GICv2, touch, macOS notes
    plans/                   display, GPU, wireless, audio, standalone plans
    research/                ABOX, DPU, Wi-Fi and Bluetooth reports
    architecture.md
    building.md
    hardware.md
  kernel/
    xnu/                     redwood.patch, files/, apply.sh, export.sh
    xnuloader/               EFI loader source
    common/                  build profiles including y2s.conf
    tools/                   Mesa, brcmfmac, APFS, HFS and diagnostic tools
    y2s.sh                   build, pack, flash and log commands
  userland/
    src/                     iOS-side programs and injected libraries
    build.sh                 root.apfs and system.apfs
    build-gpu-y2s.sh         Mesa, EAGL and Metal build on macOS
  uefi/
    Platforms/Samsung/y2sPkg Mu-Silicium platform package
    BootManagerLib/          loader hook and modified BDS boot manager
    y2s.patch                changes to Mu-Silicium
  reference/
    dts/                     decompiled stock device trees
    firmware-notes.md        vendor blob extraction
```

Not included: `xnu-src`, `ios165`, `prebuilt`, `fw-stock`, `samloader`, `backup-y2s`, firmware blobs, the system image and build output.

## Documentation

- [docs/architecture.md](docs/architecture.md): layer model, memory map, driver model, logging.
- [docs/hardware.md](docs/hardware.md): addresses, pins, buses, interrupts.
- [docs/building.md](docs/building.md): full build and troubleshooting.
- [docs/history/handoff.md](docs/history/handoff.md): original handover covering the boot chain and recovery.
- [docs/history/PROGRESS2510.md](docs/history/PROGRESS2510.md): October session notes for Metal, display, wireless and audio.
- [docs/history/GICV2.md](docs/history/GICV2.md): interrupt driver design.
- [docs/history/TOUCH.md](docs/history/TOUCH.md): touch hardware and protocol.
- [docs/history/MACOS.md](docs/history/MACOS.md): native macOS build notes.
- [docs/plans/07-display.md](docs/plans/07-display.md): display register work and run logs.
- [docs/plans/06-wireless.md](docs/plans/06-wireless.md): Wi-Fi, WPA2 and Bluetooth.
- [docs/plans/08-audio.md](docs/plans/08-audio.md): ABOX and amplifier plan.
- [docs/plans/09-standalone.md](docs/plans/09-standalone.md): boot menu, USB flashing, UFS root, HFS+ `/var`.

The history and plan files are working notes. They contain superseded statements and should not be read as current status.

## Credits and licences

Upstream projects:

- **HackiOS** by OOSEX GANG: XNU port, loader, UEFI work and userland framework. MIT.
- **Mu-Silicium** by Project Silicium: open UEFI and Exynos 9830 support. BSD-2-Clause and component licences.
- **Apple XNU** `xnu-8796.121.2`: APSL 2.0.
- **Apple HFS+** from `apple-oss-distributions/hfs`: APSL 2.0.
- **Mesa**, **Panfrost** and **libdrm**: MIT.
- **Linux brcmfmac**: ISC, with Linux and FreeBSD sources used through LinuxKPI.
- **z3s** (`Bentlybro/z3s-mainline-linux`): reference work for Panfrost, PCIe and BCM4375 on the same SoC.
- **libdeflate**: MIT.

Original code in this repository is MIT licensed under [LICENSE](LICENSE). Upstream files retain their own licences, listed in [NOTICE.md](NOTICE.md) and in file headers. No Apple iOS files, extracted Samsung firmware blobs or stock ROM images are included; driver sources inherited from HackiOS are kept as they are.

Independent research project. Not affiliated with, authorised by or endorsed by Apple, Samsung, Broadcom or Cirrus Logic. iOS, iPhone and Apple are trademarks of Apple Inc. Samsung, Galaxy and Exynos are trademarks of Samsung Electronics. Use it only on hardware you own and can recover.
