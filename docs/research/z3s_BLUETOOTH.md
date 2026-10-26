# Bluetooth (BCM4375 over UART)

The z3s Bluetooth is the BT side of the BCM4375 WiFi+BT combo (the WiFi side is on PCIe, see `WIFI_BRINGUP.md`). BT is a UART-attached HCI controller on UART1, driven by the mainline `hci_bcm` / `hci_uart` / `btbcm` stack over serdev. It comes up on boot as `hci0` (UP RUNNING) once the four pieces below are in place. This is the first Bluetooth on this SoC in mainline.

## Hardware

- UART1 at `0x10840000` (`samsung,exynos850-uart`), IRQ `GIC_SPI 416`, pins `gpq0-0..3` (ALIVE pinctrl `0x15850000`), hardware flow control.
- Control GPIOs (managed by `hci_bcm`): BT_EN/reg-on `gpm16-0`, device-wake `gpg1-1`, host-wake `gpa2-3`.
- Firmware: the BCM4375B1 patchram `.hcd` (extract from your device, see `../firmware/README.md`). No blob shipped here.

## The four things that make it work

1. **USI wrapper.** UART1 is a USI-serial-v2 block, exactly like the I2C buses, so it needs the `exynos-usi` wrapper or the USI is never muxed to UART and the core never communicates. The DT is `usi@108400c0 { compatible = "samsung,exynos850-usi"; samsung,mode = <1>; serial@10840000 { ... } }`, with the USI SFR at `uart_base + 0xc0`.
2. **SW_CONF offset.** `samsung,sysreg = <&sysreg_peric1 0x1000>`. The BT UART is USI channel `usi_uart_bt` = `/usi@10721000` in the downstream, i.e. `sysreg_peric1 + 0x1000`. A wrong offset muxes the wrong channel and nothing works.
3. **`samsung,clkreq-on`.** This was the decisive fix. The binding says it is "needed for UART mode": without it `exynos-usi` does not set the continuous-clock bit, the clock gets gated, and UART **Rx is dead** (total silence at every baud, which is a very misleading symptom). With it, Rx works and the default clock rate is fine.
4. **Firmware filename.** `hci_bcm` reads the BT core's ID, which reports as `BCM4362A2` even though the chip is `BCM4375B1`, and asks for `brcm/BCM4362A2.hcd`. Install the extracted patchram under that name (keeping `BCM4375B1.hcd` too does no harm). Then `hci_bcm` downloads the patchram and the controller finishes coming up with its real address.

The DT also carries a small flow-control workaround (a `uart1_bus_norts` pin group plus a gpio-hog that drives `gpq0-2`/nRTS low) because this UART's hardware auto-flow-control does not drive RTS, the downstream handles this with `samsung,rts-gpio-control`. Bluetooth works with it; it is likely revertible.

## Kernel and userspace

`CONFIG_BT`, `CONFIG_BT_HCIUART`, `CONFIG_BT_BCM` are `=m` (RFKILL is a module, so BT cannot be `=y`); `CONFIG_BT_HCIUART_BCM=y`, `CONFIG_BT_HCIUART_SERDEV=y`, `CONFIG_SERIAL_SAMSUNG=y`, `CONFIG_SERIAL_DEV_BUS=y`. Install `bluetooth.ko`, `hci_uart.ko`, `btbcm.ko` into `/lib/modules/<krel>/` and `depmod`. Autoload with `../rootfs/systemd/z3s-bluetooth.modules-load.conf` (drop as `/etc/modules-load.d/`) so the serdev binds on boot. `bluetoothd` handles the rest for the desktop.
