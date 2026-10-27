# Architecture

This document describes how the pieces fit together. The [README](../README.md) is the summary and the status report; this is the engineering detail.

## The four layers

```
+-----------------------------------------------------------------------+
| iOS 16.5 user space                                                   |
| launchd, backboardd, SpringBoard, apps, our init and injected libs    |
+-----------------------------------------------------------------------+
| XNU 8796.121.2                                                        |
| GICv2, arch timer, PSCI SMP, APFS, HFS+, MiniIOSurface, MiniIOMFB,    |
| AMFI/Sandbox stand-ins, minicrypto, the network KPI                   |
+-----------------------------------------------------------------------+
| Exynos 990 drivers                                                    |
| display, Mali, PCIe and BCM4375, Bluetooth UART, UFS, touch, keys,    |
| MAX77705, ACPM DVFS, ABOX and CS35L41                                |
+-----------------------------------------------------------------------+
| UEFI and loader                                                       |
| Mu-Silicium y2sPkg, BootShim, XnuLoaderMain, xnuloader                |
+-----------------------------------------------------------------------+
| Samsung sboot                                                         |
+-----------------------------------------------------------------------+
```

Nothing below XNU is Linux. XNU runs at EL1 on the phone's CPU, owns the MMU, the interrupt controller and the devices, and provides the BSD and IOKit interfaces that iOS expects.

## The boot flow in detail

1. **sboot** reads the `boot` partition if the boot-recovery flag in `misc` is clear, otherwise `recovery`. In a standalone installation both hold the HackiOS boot image. In an Android installation `boot` still holds Android's kernel, and `y2s.sh flash` writes the image to `recovery` and reboots into it.
2. **The boot image** is an Android boot image whose kernel payload is `BootShim` plus a 2 MiB UEFI firmware volume. `BootShim` copies the firmware volume to `0xE8000000` and jumps to its entry point. The image header's `image_size` is exactly 2 MiB. Nothing is appended after the firmware volume.
3. **Mu-Silicium** starts its DXE core, memory map, console, UFS, USB and keypad drivers. `BdsDxe` runs and finds no boot option on this phone. The patched `PlatformBootManagerUnableToBoot()` calls `XnuLoaderMain()` instead of halting.
4. **XnuLoaderMain** looks at `0xE8100000`, inside the firmware volume's unused tail, for the XNUP container. It finds `loader.efi`, calls `LoadImage()` and `StartImage()`, and cold-resets if the loader returns without starting a kernel.
5. **xnuloader** is a freestanding AArch64 UEFI application linked at `0xA1000000`. It:
   - prints its own diagnostics to the UEFI console;
   - finds the display and records the framebuffer base and size;
   - reads the XNUP payload entries for the kernel, the small root image and the system image;
   - reads the kernel into a 16 MiB staging window at `0xF0000000`, below the display at `0xF1000000`;
   - reserves the Wi-Fi DMA pool and the display pool below 4 GB;
   - builds a flattened device tree and `boot_args`;
   - maps a physical window for XNU;
   - exits boot services, turns the MMU off and jumps to the kernel entry point.
6. **XNU** initialises the platform, starts the secondary cores through PSCI, mounts APFS, starts `launchd` from the root volume, and the iOS user space takes over.
7. **Returning** to the loader is a deliberate path. A key hold or touch gesture calls `loader_return()`, which stops the other cores, writes the kernel log to `recovery`+16 MiB, quiesces the display, and returns to the loader's menu.

## Memory map

These are the addresses used by the port. The UEFI memory map comes from `y2sPkg/Library/MemoryMapLib/MemoryMapLib.c` and the S5E9830 platform package.

| Region | Address | Notes |
|---|---|---|
| RAM base | `0x80000000` | 12 GB installed |
| UEFI firmware volume | `0xE8000000` | 2 MiB |
| DXE heap | `0xE8200000` | |
| XNUP payload | `0xE8100000` | inside the FV tail, 512 KiB maximum |
| UEFI stack | `0xEBE00000` | |
| XNU download window | `0xF0000000` | 16 MiB, ends at the display |
| Display reserved | `0xF1000000` | 1440x3200, 5760 bytes per row |
| EL2 reservations | `0x83C80000`, `0xC1400000` | Samsung hypervisor |
| S2MPU table | `0xC1800000` | |
| Kernel physical load | about `0x883000000` | above 4 GB |
| Display pool | below 4 GB, from the loader | 64 MiB |
| Wi-Fi DMA pool | below 4 GB, from the loader | 32 MiB |

The fact that XNU's memory is above 4 GB matters. The BCM4375 and the display controller cannot DMA there, so the loader reserves pools below 4 GB and publishes their addresses in the device tree's `/chosen` node.

## The XNU overlay

`kernel/xnu/` is the durable form of the kernel work:

```
kernel/xnu/
  redwood.patch        changes to files that already exist in Apple's tree
  files/               new files at their paths in the XNU tree
  apply.sh             fetch Apple's xnu-8796.121.2 and apply everything
  export.sh            reverse: regenerate the patch and the files from a working tree
  README.md            the upstream kernel README, with the build details
```

`apply.sh` clones `apple-oss-distributions/xnu` at tag `xnu-8796.121.2`, applies the patch, and copies `files/` over the tree. `export.sh` does the opposite. A working tree is an ordinary git checkout on a branch called `redwood`.

The overlay contains both generic HackiOS support code and the y2s drivers. The generic code includes APFS, ramfs, the Mini drivers, the input core, the loader return path and the QEMU virtio devices. The y2s code is concentrated in:

| File | Purpose |
|---|---|
| `osfmk/arm64/gic_v2_irq.c` | GICv2 interrupt controller |
| `bsd/dev/redwood_sec_ts.c` | Samsung `sec_ts` touch |
| `bsd/dev/redwood_exynos_keys.c` | GPIO power and volume keys |
| `bsd/dev/redwood_max77705.c` | MAX77705 fuel gauge and charger |
| `bsd/dev/redwood_bbi2c.c` | shared bit-banged I2C bus |
| `bsd/dev/redwood_exynos_pcie.c` | Exynos PCIe host |
| `bsd/dev/redwood_bcm4375.c` | BCM4375 Wi-Fi |
| `bsd/dev/lx_wpa.c` | WPA2 four-way handshake in the kernel |
| `bsd/dev/redwood_exynos_bt.c` | BCM4375 Bluetooth over USI16 |
| `bsd/dev/redwood_exynos_sound.c` | ABOX and CS35L41 audio path |
| `bsd/dev/redwood_exynos_acpm.c` | ACPM DVFS |
| `bsd/dev/redwood_ufshc.c` | Exynos UFS host controller |
| `bsd/dev/redwood_ufs.c` | block device, root window and write policy |
| `bsd/dev/redwood_mali.c` | Mali-G77 and the Panfrost UAPI |
| `iokit/Kernel/RedwoodMali.cpp` | IOKit user client for the GPU |
| `iokit/Kernel/MiniIOMobileFramebuffer.cpp` | display, tear control, brightness, scaler |
| `bsd/dev/redwood_logd.c` | kernel log ring for the Wi-Fi log daemon |
| `bsd/dev/redwood_perf.c` | per-core clocks and frame statistics |

The kernel is configured as `BCM2837`, which is a generic ARM64 configuration in XNU, not a Raspberry Pi dependency. `GIC_V2=1` adds `-DGENERIC_GIC_V2_IRQ=1` to the machine flags. The build system does not notice a change to a `-D` on its own, so a switch between GICv2 and GICv3 needs the dependent objects touched or the build directory removed.

## Driver model

There is no device tree binding layer like Linux's. The loader writes a device tree because XNU's ARM platform code expects one, but the y2s drivers generally find their hardware through a boot argument and a fixed address from the device tree.

A typical driver follows this shape:

1. A boot argument, for example `touch_ts=1`, `wifi_pcie=4`, `mali=1`, `snd=1`, `dvfs=2`, enables it.
2. A probe function maps the registers with `ml_io_map()` or `ml_io_map_unmappable()`, reads an ID register, and refuses to continue if the ID is wrong.
3. A kernel thread polls or waits on an interrupt. The y2s drivers are deliberately polled where an interrupt would add risk; the Wi-Fi driver polls its mailbox, and the touch driver polls the attention GPIO.
4. Status goes to the log through `loader_log_line()` or a prefixed `printf`. The log ring is readable over Wi-Fi and is saved by the loader.
5. The driver reports into an existing iOS service where one exists: `redwood_input.c` for touch and keys, `RedwoodBattery` for power, `redwood_net.c` for `en0`, `MiniIOSurface` and `MiniIOMobileFramebuffer` for graphics.

Registers are not assumed to be live. A write-back test before the real work catches a block that is not clocked or not powered. This is the single most useful habit in the port: it separates a wrong offset from a block that is switched off.

## Boot arguments

The command line is stored in the XNUP payload, not compiled into the kernel. `./y2s.sh payload "<args>"` changes it without a rebuild.

| Argument | Effect |
|---|---|
| `cpumask=0xff` | use all eight cores (`0x3f`: the four A55 and the two A76) |
| `cpus=N` | use N cores; never combine with `cpumask` |
| `touch_ts=1` | probe and poll the touch controller (2: probe only, 4: reset the bus) |
| `bms=1` | MAX77705 battery and charger |
| `mali=1` | start the Mali and publish the GPU service |
| `mali_irq=1` | use the GPU job interrupt |
| `mali_cached=1` | use cached GPU memory |
| `fb_te=1` | tear-controlled copy path |
| `fb_scaler=1` | zero-copy display scaler |
| `fb_bl=1` | brightness and display power through DSIM |
| `fb_threads=N` | helper threads for the copy path |
| `dvfs=2` | ACPM DVFS, the CPU and GPU governor and thermal steps |
| `dvfs_cl2=2304000` | top clock of the M5 cluster |
| `jit_flush=1` | trap user cache maintenance so JIT code is coherent on every core |
| `cpuidle=1` | power idle cores down through PSCI `CPU_SUSPEND` |
| `metal=1`, `camtl=1` | publish the Metal plug-in; CoreAnimation renders with Metal |
| `metal_profile=1`, `mediamtl=1` | Metal capability profile; Metal for `mediaserverd` |
| `autounlock=0` | start at the lock screen |
| `fb_hot=C` | cap brightness from C degrees (default 90, `0` off) |
| `touch_lat=1` | per-frame and touch timing lines |
| `perf=1` | per-core clocks and boot timing |
| `wifi_pcie=1..4` | PCIe look, link, DMA grants, full Wi-Fi |
| `wifi_pcie_wait=N` | seconds before the PCIe work starts |
| `bt=1` | Bluetooth UART and HCI bring-up |
| `snd=1` | ABOX and amplifier bring-up |
| `ufs=2` | use the kernel's UFS driver |
| `ufs_native=1` | native command list instead of the loader's |
| `ufs_root=super:OFFSET:SIZE` | mount the iOS root from `super` |
| `ufs_write=1` | allow writes in the userdata window |
| `var_hfs=1` | create or mount HFS+ on userdata as `/private/var` |
| `rd=md0` | tell launchd the root is the resident image |
| `debug=0x8` | XNU debug flags |

## Logging

There are three channels, and they answer different questions.

- **The console log ring** is written by `loader_log_line()` and by the display driver's `fbSay()`. It is the reliable channel: it survives a return to the loader and is served by `rwlogd` while iOS runs.
- **The kernel message buffer** receives ordinary `printf` and `IOLog` output. It is smaller and wraps, so a plain `printf` can disappear before a log is pulled. `redwood_logd.c` publishes both the ring and the message buffer.
- **The loader's own console** shows the UEFI and loader diagnostics before XNU starts. It shows only the tail on a long log, which is why the loader writes the full log to storage before printing it.

**The m2m channel** (`bsd/dev/redwood_m2m.c`, `/dev/m2m`, `sysctl kern.m2m_log`) holds structured records from the Metal driver, the GPU, the display, power and input, `<seq> <seconds> <severity> <CATEGORY> <process>[<pid>] <message>`. `rwlogd` serves it at `/m2m`, and `./y2s.sh loop` saves and compares it run by run ([plans/10-mali2metal.md](plans/10-mali2metal.md)).

`./y2s.sh log` reads the saved log from `recovery`+16 MiB over adb, strips NUL bytes and writes `../xnulog.txt`. `./y2s.sh wlog` fetches it over HTTP from the running phone. `./y2s.sh ulog` fetches it over USB from the boot menu.

## Why the boot arrangement is unusual

Most Android ports use `fastboot boot` and keep the system image in RAM. Samsung does not expose that path. The port therefore borrows the recovery partition as a small disk and keeps the iOS root in the unused tail of `super`. This has three consequences:

- The flashing loop is a `dd` from Android or a USB write from the loader, not a fastboot download.
- The recovery partition doubles as the log store, so the log and the kernel must not overlap.
- The kernel slot is 16 MiB, and the kernel is already close to it. Storage, HFS+ and the GPU have filled most of the space that the original 15 MB kernel left.

The design is documented with measurements in [history/Y2S-PROGRESS.md](history/Y2S-PROGRESS.md) and [plans/09-standalone.md](plans/09-standalone.md).
