# Notices and upstream licences

The MIT licence in `LICENSE` covers the original work in this repository. Several directories contain code from other projects, and those files keep their own licences.

## Apple XNU and HFS+

`kernel/xnu/redwood.patch` and `kernel/xnu/files/` are a derivative work of Apple's XNU `xnu-8796.121.2` and, in `bsd/hfs/`, of Apple's HFS+ sources from `apple-oss-distributions/hfs`. Both are released under the Apple Public Source License 2.0. A copy of that licence is in `kernel/xnu/` and in the headers of the affected files. The APSL permits redistribution of modified source, provided the licence and notices are kept.

No Apple iOS user-space files, dyld shared cache, kernel caches or IPSW contents are included. The build fetches Apple's IPSW from Apple's CDN on the user's own machine.

## HackiOS

The loader, the userland framework and much of the kernel support code come from HackiOS by OOSEX GANG:

- https://github.com/oosexgang/hackios-xnu-kernel
- https://github.com/oosexgang/hackios-xnu-userland

HackiOS is released under the MIT licence. The original copyright notice is retained in the affected files. The y2s-specific drivers and documentation in this repository are additions or modifications on top of that work.

## Mu-Silicium and Project Silicium

`uefi/` contains the `y2sPkg` platform package, the loader hook and a patch against Mu-Silicium. Mu-Silicium is released under the BSD 2-Clause licence, with additional notices from Apple, ARM, Microsoft, Intel, Linaro, the Android Open Source Project and other contributors. Those notices are retained in the Mu-Silicium tree. The UEFI binary is not included.

## Mesa, Panfrost and libdrm

`kernel/tools/mesa/` contains patches and build scripts for Mesa, including the Panfrost driver. Mesa and Panfrost are MIT licensed. The libdrm stand-in under `kernel/tools/mesa/redwood-drm/` is part of the same work. Mesa itself is downloaded and built by `mk-mesa.sh`; its sources are not vendored.

## Linux brcmfmac and LinuxKPI

The BCM4375 driver draws on Linux's `brcmfmac`, which is ISC licensed, and on the Linux and FreeBSD sources used by the LinuxKPI layers. The patches in `kernel/tools/` and the notes in `docs/` retain their notices. The Linux source archives are downloaded at build time; they are not vendored.

## z3s and other reference work

The documentation cites `Bentlybro/z3s-mainline-linux`, a GPL-2.0 Linux tree for the same SoC, and Samsung's GPL-2.0 kernel sources. They are used as references. No GPL source is linked into XNU. The reports in `docs/research/` are summaries written for this port, not copies of those trees.

## itlwm

The Apple80211 structures used for the Wi-Fi signal and noise reports were checked against the published headers and driver of `OpenIntelWireless/itlwm`. It is used as a reference only. No itlwm code is included.

## libdeflate

`kernel/xnu/files/bsd/miscfs/apfs/libdeflate/` is MIT licensed. The licence text is kept beside the sources.

## Firmware and device data

This repository does not include the phone's stock firmware images, the extracted Samsung vendor blobs, the iOS system image or the phone's partition backups.

The port needs the following files, which the user must extract from hardware they own:

- Wi-Fi firmware, CLM and NVRAM for the BCM4375.
- Bluetooth patchram for the BCM4375.
- Audio firmware and tuning data for the ABOX and CS35L41, if the DSP path is used.
- The iOS 16.5 IPSW, downloaded from Apple.
- A stock Samsung AP tar for recovery.

`kernel/xnu/files/bsd/dev/redwood_focaltech_fw.h` is part of the upstream HackiOS touch driver and contains a FocalTech firmware array. It is retained because it is part of that driver, not because it is used on y2s. The y2s touch controller is a Samsung S6SY79A and uses `kernel/xnu/files/bsd/dev/redwood_sec_ts.c`.

## Trademarks

iOS, iPhone, iPad, macOS and Apple are trademarks of Apple Inc. Samsung, Galaxy and Exynos are trademarks of Samsung Electronics. Broadcom and BCM are trademarks of Broadcom Inc. Cirrus Logic and CS35L41 are trademarks of Cirrus Logic, Inc. Other names may be trademarks of their respective owners.

This project is independent research. It is not affiliated with, authorised by or endorsed by any of those companies.
