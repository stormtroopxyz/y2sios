# BCM4375 onboard Wi-Fi bring-up (SOLVED)

Status: fully working. The onboard Broadcom BCM4375 associates, completes the WPA2 4-way handshake, gets a DHCP lease, routes to the internet, and was confirmed with real web browsing. This is the first full WPA2 connection by this SoC's BCM4375 on a mainline kernel. This document is the complete story: the PCIe controller and brcmfmac port, the S2MPU DMA work, the interrupt path, and the final `host_cap` fix that made the handshake work. It stays on the working mainline 6.12 port; the Samsung 4.19 tree is a hardware and ABI reference only.

## Hardware wiring (PCIe endpoint, not SDIO)

The onboard Wi-Fi is a PCIe endpoint on Exynos PCIe controller 0, enumerated as Broadcom `14e4:4475` (subsystem `14e4:4375`). The stock device tree enables controller 0 with ELBI at `0x133b0000`, PHY at `0x133e0000`, DBI at `0x133f0000`, PCS at `0x133d0000`, a single lane, WLAN-enable on `gpb0-4`, and WLAN host-wake on `gpa3-7`. This controller uses Samsung's older `host-v0` ELBI interrupt layout (not the newer Exynos PCIe RC blocks): INTx pulse status and clear are at ELBI `0x000` (bits 0/2/4/6), pulse enable at ELBI `0x00c`, and the MSI rising pulse, when used, is bit 29. The stock Wi-Fi overlay explicitly sets `use-msi = "false"`, so Samsung runs the BCM4375 through its wired legacy INTA path.

## Firmware

Installed under `/lib/firmware/brcm/`: `brcmfmac4375-pcie.bin`, `brcmfmac4375-pcie.clm_blob`, and `brcmfmac4375-pcie.txt` (the production Semco NVRAM variant). See `../firmware/README.md` for how to extract these from your own device, since none are shipped here. The observed firmware version is `RTE (SDIO-PCIE-MSGBUF) 18.41.117 (B1 Network/rsdb) on BCM4375 r5`. The missing optional `txcap` and board-specific `brcmfmac4375-pcie.samsung,z3s.bin` messages are not a blocker: the generic firmware fallback succeeds and both radios attach. The interface comes up with the EFS-provisioned MAC.

## Kernel and brcmfmac port

The port adds `CONFIG_PCI_EXYNOS`, the Exynos9830/990 HSI1 clocks plus PHY calibration and the PCIe controller device-tree node, `CONFIG_BRCMFMAC_PCIE`, the PCI ID `0x4475`, chip ID 4375 with firmware basename `brcmfmac4375-pcie`, the BCM4375 RAM base `0x170000`, and the Exynos990 HSI1 S2MPU DMA grants. The staged source is under `07_wifi_port/active6.12/` in the working tree; the public copies are in `../kernel/drivers/pcie-exynos/` and `../kernel/drivers/wifi-brcmfmac/`.

## Exynos990 S2MPU DMA grants

HSI1 PCIe DMA is protected by Samsung's stage-2 memory protection unit. A normal DMA mapping is not sufficient: the HSI1 subsystem must also be granted access to every 64 KiB physical granule the endpoint uses. The ABI was proven first with a one-shot diagnostic module: HVC `0xc6000101`, subsystem argument `(1 << 16) | 24 = 0x10018` (subsystem index 24 is `hsi1`), 64 KiB granularity, with grant and revoke both returning 0. The brcmfmac implementation substitutes coherent alloc/free and streaming map/unmap inside brcmfmac only, grants each covered 64 KiB granule, reference-counts overlapping grants, revokes on the last release, and is restricted to the exact Broadcom `14e4:4475` endpoint. The four wrappers are `brcmf_dma_alloc_coherent`, `brcmf_dma_free_coherent`, `brcmf_dma_map_single`, and `brcmf_dma_unmap_single`.

Samsung firmware also requires its bookkeeping table in reserved RAM. The live device tree reserves `0xc1800000..0xc1afffff` (3 MiB, no-map), confirmed at boot as `OF: reserved mem:... (3072 KiB) nomap non-reusable s2mpu-table@c1800000`. Never issue the HVC grants on a boot without that reserved table. With the table and grants in place the earlier firmware-side `H2D DMA data transfer error` and ThreadX trap disappear and the firmware reaches `ThreadX v5.6 initialized`, which proves firmware download, RAM base selection, and protected DMA are all correct.

## Interrupt path: legacy INTx, not MSI

Generic DesignWare host initialization creates an MSI domain, and upstream brcmfmac calls `pci_enable_msi()` without checking the result, so on this port it unexpectedly succeeds and switches the BCM4375 away from Samsung's stock legacy INTA route. The shipping fix keeps only device `0x4475` on legacy INTx (every other brcmfmac PCIe device retains its existing MSI behaviour), because that matches Samsung's production device tree and avoids needing a separately S2MPU-authorized DesignWare MSI target. See `../kernel/drivers/wifi-brcmfmac/0002-bcm4375-use-legacy-intx.patch`. A host-v0 MSI candidate (ELBI pulse bit 29 dispatching `dw_handle_msi_irq()`) was also built for reference but is not the shipping path.

With INTx, firmware preinit completes, `wlp1s0` is created, and IRQ 52 carries the device interrupt. A fresh-boot scan found 38 BSS entries across 2.4 and 5 GHz, and IRQ 52 incremented during the scan, which proved the interrupt path and the firmware event path both work.

## The final blocker: host_cap (the fix)

Everything above came up, but the WPA 4-way handshake still timed out (AP deauth reason 15, `rx_packets = 0`). The prior read of this was a broken RX or interrupt path. That was wrong: scans returned APs and IRQ 52 was serviced, so events worked. The real gap is that mainline brcmfmac never advertises host capabilities to the dongle. The vendor Samsung driver (`bcmdhd_101_12`) writes a `host_cap` word to the PCIe shared area in `dhd_set_host_cap()` for shared-protocol rev 6 or higher. This firmware is PCIE shared version 7, and without that advertisement it never DMAs received data frames into the host RX buffers, so EAPOL-Key M1 never reaches wpa_supplicant and the 4-way cannot start. dcmd and IOCTL responses kept working because they are polled, and scan results kept working because they arrive via the event path, which together masked the data-RX gap.

The fix, in `brcmf_pcie_init_share_ram_info()` (`drivers/.../brcmfmac/pcie.c`), right after reading `ring_info_addr`:

```c
if (shared->version >= 6) {
        u32 host_cap = shared->version |   /* active_ipc_version (=7) low byte */
                       0x00000400 |        /* HOSTCAP_H2D_ENABLE_HOSTRDY   */
                       0x00001000 |        /* HOSTCAP_DS_NO_OOB_DW         */
                       0x00020000 |        /* HOSTCAP_EXTENDED_TRAP_DATA   */
                       0x00800000;         /* HOSTCAP_UR_FW_NO_TRAP        */
        brcmf_pcie_write_tcm32(devinfo, sharedram_addr + 84, host_cap);
}
```

The value is `0x00821407` for version 7. The offset is the shared-struct base plus 84, which is the `host_cap` field, verified against the vendor `pciedev_shared_t` in bcmpcie.h (flags at 0, ring_info at 48, flags2 at 80, host_cap at 84). Every mainline offset define (max_rxbufs at 34, rx_dataoffset at 36, h2d_mb at 40, d2h_mb at 44, ring_info at 48) matches this struct walk exactly. It is written early in probe, before `brcmf_pcie_hostready()` signals the firmware, matching the vendor ordering. The bits mirror what mainline actually supports: HOSTRDY (brcmfmac rings DB1), DS_NO_OOB_DW (no inband or OOB deep-wake in mainline, so DS is disabled), and EXTENDED_TRAP_DATA plus UR_FW_NO_TRAP (always). HWA, IDMA, IFRM, DAR, HSCB, and H2D_PHASE are deliberately omitted because mainline does not implement them.

## Proof (password-free A/B test)

I associated to the AP with a deliberately wrong PSK. In WPA2-PSK the AP sends M1 to any associated STA regardless of key, so this exercises the exact RX-data path with no real credential.

| module | RX pkts | EAPOL M1 delivered? | outcome |
|--------|--------:|---------------------|---------|
| known-good `fd1df1e1` (no host_cap) | 0 | never arrives | wpa self-deauths reason 3, no handshake |
| patched `854e3bd1` (host_cap) | 6 | `WPA: RX message 1 of 4-Way Handshake` | sends M2, AP rejects wrong key (reason 15), normal |

The only reason the patched run did not fully connect is the intentionally-wrong PSK. With the correct key, M3 and M4 follow and it completes. There is no remaining driver gap in the handshake path.

## Verified end-to-end

Connected with the real PSK, confirmed over the USB link: `wpa_state = COMPLETED`, `key_mgmt = WPA2-PSK`, pairwise and group CCMP, a DHCP lease on the Wi-Fi LAN, NetworkManager reporting `wlp1s0: wifi: connected`, a 5 GHz link on channel 116 at -41 dBm with a 1200 Mbit/s tx rate (Wi-Fi 6, 80 MHz), a ping to the LAN gateway at 3/3 with 0% loss, a ping to a public host (`1.1.1.1`) at 3/3 with 0% loss and about 18 ms, and a working web browser. The full chain is proven: associate, EAPOL M1 delivered, 4-way handshake completed, DHCP lease, routed internet, real web traffic.

## Build, deploy, rollback

The module was built on a native arm64 host against the tree that matches the phone kernel `6.12.0-rc5-next-20241104-g75138397b8f8-dirty`, with `nice -n 19 make -j3 M=drivers/net/wireless/broadcom/brcm80211/brcmfmac modules`. The built module sha256 is `854e3bd15005fed845b4f6850f3e773d447588fc4788dbb3e135d0cf194613a6`, vermagic `6.12.0-rc5-next-20241104-g75138397b8f8-dirty SMP preempt mod_unload aarch64`. It is installed at `/lib/modules/<krel>/kernel/drivers/net/wireless/broadcom/brcm80211/brcmfmac/brcmfmac.ko` with `depmod -a` run, and survives reboot. The known-good rollback module is preserved beside it as `brcmfmac.ko.knowngood-fd1df1e1` (sha256 `fd1df1e1615dd3a2399e65037a91910bff27db5207e099d826ebd7b6e8447995`). To revert: `cp brcmfmac.ko.knowngood-fd1df1e1 brcmfmac.ko && depmod -a && reboot`, or hot-swap with `rmmod brcmfmac_wcc brcmfmac; insmod <path>; modprobe brcmfmac_wcc`. The proven module is archived in the repo at `../kernel/prebuilt-modules/brcmfmac-hostcap-854e3bd1.ko`. Being a rootfs `.ko` swap, the fix needs no reflash.

Remaining and minor: `brcmf_p2p_create_p2pdev: timeout` still logs on probe (async P2P-dev iface creation only, cosmetic, does not affect STA connectivity or NetworkManager), and the `Direct firmware load... samsung,z3s.bin /.txcap_blob failed (-2)` lines are benign board-variant probes before the generic firmware loads.

## Suspend lockup fix (found during this work)

An apparent random phone lockup with USB loss turned out to be deterministic PowerDevil suspend (AC 15 minutes, battery 10 minutes, low battery 5 minutes): the gadget disconnected almost exactly 16 minutes after boot. The current mainline Exynos990 port exposes `freeze`, `mem`, and `disk` but its resume path is not reliable. The persistent fix masks `sleep.target`, `suspend.target`, `hibernate.target`, and `hybrid-sleep.target`, disables all systemd sleep modes via `/etc/systemd/sleep.conf.d/10-z3s-no-suspend.conf`, and sets PowerDevil `SuspendSession` to `idleTime=0` and `suspendType=0` for the AC, battery, and low-battery profiles. Power-button handling and Plasma DPMS blanking are left unchanged, so the OLED can still blank without suspending Linux. After the fix the phone stayed reachable well beyond the former 15-minute trigger, and the masks and values survived reboot.

## Safety rules

Keep `usb0` explicitly unmanaged before enabling NetworkManager, because USB networking is the recovery and SSH path. Never use `/dev/mem` for this bring-up. Never extract a rootfs tar archive at `/`. Flash only through a tool that checks partition identities and LBAs, makes a full rollback image, and verifies exact readback. Only ever write `lk3rd`, `boot`, `userdata`, and microSD; writing S-Boot, PIT, modem, or efs will brick the device.
