# Documentation

The main technical account is the [README](../README.md). These documents go deeper into particular areas.

## Start here

- [architecture.md](architecture.md): the layers, the boot flow, the memory map and how a driver is added.
- [hardware.md](hardware.md): addresses, pins, buses and interrupts for the Exynos 990 S20+.
- [building.md](building.md): a full build from upstream sources, including the UEFI in a Linux VM.
- [history/](history/): the original handover and port log, kept as written.
- [plans/](plans/): the working plans for display, GPU, wireless, audio and standalone operation.
- [research/](research/): hardware reports on the ABOX, the display controller, Wi-Fi and Bluetooth.

## A note on dates

The files in `history/` and `plans/` are working notes. They were written during the port and contain statements that were later superseded. For example, `handoff.md` says that Wi-Fi has not started and that the GPU cannot be accelerated; both were written before the Mali and BCM4375 work. The current summary is the status table in the [README](../README.md).

| Date | Milestone |
|---|---|
| 23 to 24 October 2026 | UEFI, loader and XNU boot on the phone; iOS 16.5 reaches SpringBoard; touch works |
| 24 to 25 October 2026 | Display tear fix, brightness and screen power; GPU through Panfrost; Wi-Fi bring-up; Bluetooth design |
| 25 October 2026 | Zero-copy display scaling; UFS root and HFS+ `/var`; boot menu and USB flashing; Wi-Fi with WPA2 and DHCP |
| 26 October 2026 | Bluetooth HCI and `bluetoothd` attach; ACPM DVFS; audio path programmed but silent |
