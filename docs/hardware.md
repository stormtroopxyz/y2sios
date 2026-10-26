# Hardware reference

Every address here was taken from the phone's stock device tree, from Samsung's kernel sources, or from a register read on the device. The decompiled trees are in [../reference/dts](../reference/dts). When a value conflicts with a vendor source tree, the device tree wins.

## SoC and CPU

| | |
|---|---|
| SoC | Samsung Exynos 990, S5E9830 |
| CPU clusters | `ananke`: 4x Cortex-A55, `enyo`: 2x Cortex-A76, `lion`: 2x Mongoose M5 |
| MPIDRs | `0x000`, `0x001`, `0x002`, `0x003`, `0x004`, `0x005`, `0x100`, `0x101` |
| Boot core | MPIDR 0, Cortex-A55, `MIDR 0x412FD050` |
| A76 MIDR | `0x413FD0B1` |
| Architecture timer | 26 MHz; `CNTFRQ_EL0` reads zero |
| PSCI | `arm,psci-1.0`, `method = "smc"` |

## Interrupt controller

The Exynos 990 has a GIC-400. It is a GICv2, not a GICv3.

| Block | Address | Size |
|---|---|---|
| GICD, distributor | `0x10101000` | `0x1000` |
| GICC, CPU interface | `0x10102000` | `0x1000` |
| GICH, hypervisor | `0x10104000` | `0x2000` |
| GICV, virtual CPU interface | `0x10106000` | `0x2000` |

`IIDR` reads `0x0202143b`. The driver is `kernel/xnu/files/osfmk/arm64/gic_v2_irq.c`. The distributor's `GICD_CTLR` uses the non-secure group 1 enable bit, not the GICv3 affinity routing bit. SGIs go through `GICD_SGIR`, and SPIs are targeted with the `GICD_ITARGETSR` byte.

## Display

| Block | Address | Notes |
|---|---|---|
| DECON_F | `0x19050000` | main display controller |
| DSIM0 | `0x190C0000` | MIPI DSI |
| DSI DPHY | `0x190E0000` | |
| DPU DMA common | `0x19090000` | version `0x05000000` |
| IDMA channel c | `0x19091000 + 0x1000 * c` | c = 0 to 5 |
| DPP channel c | `0x19041000 + 0x1000 * c` | colour pipe and scaler |
| Framebuffer | `0xF1000000` | 1440x3200, 5760 bytes per row |
| Panel | Samsung `s6e3hab` | command mode, DSC 3:1, TE triggered |

Important DECON_F registers:

| Offset | Register | Notes |
|---|---|---|
| `0x00` | `GLOBAL_CONTROL` | bit 5 idle, bit 4 run, bit 8 command mode |
| `0x4C` | `INTERRUPT_PENDING` | bit 12 frame start, write one to clear |
| `0x60` | `SHADOW_REG_UPDATE_REQ` | |
| `0x70` | `HW_SW_TRIG_CONTROL` | bit 0 hardware trigger enable, bit 4 mask |
| `0x2A0` | `FRAME_COUNT` | real frame counter |
| `0x474` | `OUTFIFO_COUNTER0` | bits 16 to 29, output line |

DSIM0 registers:

| Offset | Register | Notes |
|---|---|---|
| `0x0C` | `LINK_STATUS1` | |
| `0x4C` | `CONFIG` | |
| `0x50` | `INTSRC` | bit 18 RX done |
| `0x58` | `PKTHDR` | header, bit 24 BTA for a read |
| `0x5C` | `PAYLOAD` | four bytes per write |
| `0x60` | `RXFIFO` | |
| `0x68` | `FIFOCTRL` | bit 10 header empty, bit 11 header full, bit 9 payload full, bit 12 RX empty |
| `0x80` | `CMD_CONFIG` | |

Panel commands used by the port:

| Command | Meaning |
|---|---|
| `F0 5A 5A` | level 2 key on |
| `F0 A5 A5` | level 2 key off |
| `B1 hi lo` | AOR, the dimming value |
| `6A hi lo 01` | DBV |
| `53 00` / `53 C0` | high brightness mode off / on |
| `F7 0F` | latch |
| `28` / `29` | display off / on |
| `10` / `11` | sleep in / out |

Samsung's level 1 key is `9F A5 A5`, level 2 is `F0 5A 5A`, level 3 is `FC 5A 5A`. Registers such as AOR and DBV read as zero unless a key is on.

## GPU

| | |
|---|---|
| GPU | Mali-G77 MP11, Valhall |
| Registers | `0x18500000`, size `0x5000` |
| `GPU_ID` | `0x90800011` |
| Shader cores | 11 (`0x670077`) |
| SYSREG_G3D | `0x18420000` |
| Job IRQ | SPI 301, GIC ID 333 |
| MMU IRQ | SPI 302, GIC ID 334 |
| GPU IRQ | SPI 300, GIC ID 332 |
| Power domain | PMU `0x15860000`, config `+0x2200`, status `+0x2204`, output `+0x2220` |
| Power domain SMC | `smc(0x82000410, 1, 0x18410204, 2)` |
| CMU_TOP PLL_G3D | `0x1A330100` |
| CMU_G3D | `0x18400600` |
| DVFS | ACPM channel for domain 10 |

The GPU starts at 260 MHz and reaches 702 MHz through ACPM. The port's Mesa patch adds a G77 model row with a 32768-byte tilebuffer. A 16384-byte tilebuffer produces `DATA_INVALID_FAULT`.

## Storage

| | |
|---|---|
| Controller | UFSHCI at `0x13100000` |
| UniPro | `0x13180000` |
| Capabilities | `CAP 0x1303ff1f`, 32 slots, 64-bit addressing |
| Version | `0x00000300` |
| Vendor ID | `0x00000105` |
| Product ID | `0x000001ce` |
| Root volume | `super` at `0x184B00000`, 2.32 GiB |
| Data volume | `userdata`, LBA 2850304, 28382208 blocks |

Exynos quirks handled by `redwood_ufshc.c`:

- Response and PRDT offsets and lengths are in bytes, not 32-bit words.
- PRD entries are at most 4 KB.
- `UTRLCLR` is cleared by writing ones.
- Every slot's bit in `UTRL_NEXUS_TYPE` must indicate a SCSI command. The vendor register is at `0x1140`.
- The doorbell bit is not taken back while `IS.UTRCS` is set. The driver retires a command by clearing `UTRCS` and the slot's `UTRLCNR` bit, then `UTRLCLR` only if needed.

## PCIe and Wi-Fi

| Block | Address |
|---|---|
| ELBI | `0x133B0000` |
| PCS | `0x133D0000` |
| PHY | `0x133E0000` |
| DBI | `0x133F0000` |
| Configuration window | `0x11FFE000` |
| Memory window | `0x11000000` |
| PMU PCIe PHY isolation | `0x15860718` |
| CMU_HSI1 | `0x13000000` |
| `WLAN_REG_ON` | `gpb0-4` |
| `PERST#` | `gpf0-1` |
| `CLKREQ` | `gpf0-0` |
| Host wake | `gpa0-7`, `gpa3-7` |

The endpoint is `14e4:4475`, a BCM4375 rev 5. The firmware is loaded into the chip's TCM at RAM base `0x170000`. The shared area is version 7, and `host_cap` is set to `0x00821407`. The DMA pool is 32 MB below 4 GB and is granted to the hypervisor in 64 KB granules with `hvc(0xc6000101, (1 << 16) | 24, granule, 0x10000, 3)`.

## Bluetooth

| | |
|---|---|
| UART | USI16, `serial_16` at `0x10650000` |
| Pins | from the device tree's `bt` node |
| `BT_EN` | `gpm16-0` |
| Device wake | `gpg1-1` |
| Host wake | `gpa2-3` |
| First speed | 115200 baud |
| Running speed | 3 Mbaud |
| Patchram | `bcm4375B1-semco.hcd` |
| BD address command | `0xfc01` |

The UART clock is 200 MHz after the driver programs USI16. The HCI reset, local version and patchram sequence are in `redwood_exynos_bt.c`.

## Audio

| Block | Address |
|---|---|
| ABOX | `0x18C50000` |
| CMU_AUD | `0x18C00000` |
| `pd-aud` | PMU `0x15861900` |
| Power domain SMC | `smc(0x82000410, 1, 0x18e10204, 2)` |
| UAIF2 | `0x18C50520` |
| ROUTE_CTRL0 | `0x18C50040` |
| RDMA3 | ABOX register block |
| Left amplifier | CS35L41 at I2C `0x41` |
| Right amplifier | CS35L41 at I2C `0x40` |
| Amplifier bus | `hsi2c_31` |
| DMA ring | 32 KB in the loader's DMA pool |

The audio clock tree: PLL_AUD0 at 1179.648 MHz, AUDIF divided by 16 to 73.728 MHz, UAIF2 BCLK divided by 24 to 3.072 MHz. The amplifiers are configured as slaves on DSP_A with ASPRX1 routed to the DAC.

## Touch, keys and power

| | |
|---|---|
| Touch controller | Samsung S6SY79A, `sec_ts` y79a protocol |
| I2C address | `0x48` |
| Bus | bit-banged on `gpp5-6` (SCL) and `gpp5-7` (SDA) |
| Pinctrl | PERIC1 at `0x10730000` |
| Attention | `gpa1-0`, active low |
| Coordinate range | 0 to 4095 on both axes |
| `tsp_io` | S2DOS05 LDO2, 1.8 V |
| `tsp_avdd` | S2DOS05 LDO3, 3.0 V |
| S2DOS05 bus | `hsi2c_4`, `gpp1-0` and `gpp1-1`, address `0x60` |
| Volume up | `gpa0-3`, active low |
| Volume down | `gpa0-4`, active low |
| Power | `gpa2-4`, active low |
| Key pinctrl | ALIVE at `0x15850000`, `gpa0` at `+0x00`, `gpa2` at `+0x40` |

Touch power sequence: enable LDO2, wait 1 ms, enable LDO3, wait 100 ms. The enable register is `0x03`; bit 1 is LDO2 and bit 2 is LDO3. Other bits in that register feed the display and must not be cleared. LDO2 is set to `0x0C` for 1.8 V and LDO3 to `0x0C` for 3.0 V.

The keys are read directly from the GPIO data register. The power key is not read through the S2MPS19 PMIC.

## Battery and charger

| | |
|---|---|
| PMIC | MAX77705 |
| PMIC address | `0x66` |
| Charger address | `0x69` |
| Fuel gauge address | `0x36` |
| USB-C and MUIC address | `0x25` |
| Bus | `hsi2c_7` at `0x105C0000`, `gpp1-6` and `gpp1-7` |
| Battery | 4500 mAh |
| Sense resistor | 2 mOhm |
| Fuel gauge interrupt | `gpa1-6` |

The driver reads `SOCREP`, `VCELL`, `CURRENT`, `AVG_CURRENT`, `FULLCAP_REP` and the charger status registers. It does not write charger configuration. The current LSB at 2 mOhm is 312.5 uA and the capacity LSB is 1 mAh. Voltage calculations need 64-bit arithmetic because `raw * 78125` overflows 32 bits above about 4.29 V.

## ACPM and clocks

| | |
|---|---|
| ACPM shared memory | SRAM `+0x7000` |
| Plugins | 9 |
| Channels | 11 for the AP |
| A55 cluster | domain 2, up to 1.742 GHz |
| A76 cluster | domain 3, up to 2.21 GHz |
| GPU | domain 10, 260 to 702 MHz |
| Temperature sensors | big, mid, little, G3D |

The driver discovered the channels by walking the shared-memory descriptors. It sends requests through the mailbox at the addresses printed in the `dvfs:` log lines. The M5 cluster is a separate domain and is not used because of the Safari crash described in the README.

## Pin control blocks

The S5E9830 exposes seven pinctrl blocks:

```
pinctrl_0   0x15850000   ALIVE
pinctrl_1   0x15C30000
pinctrl_2   0x13040000   HSI1
pinctrl_3   0x13C30000   HSI2
pinctrl_4   0x10430000   PERIC0
pinctrl_5   0x10730000   PERIC1
pinctrl_6   0x15580000
```

Use the device tree for the exact names. Reading a block whose clock is gated can hang the bus. Two blocks are especially dangerous:

- `0x19100000` is the DPU SysMMU. Reading it has faulted the bus. Do not touch it while bringing up the display.
- HSI1 is shared between PCIe and the UFS the system boots from. Only touch the PCIe gates, and only after the boot is far enough along.

## Sources

- `reference/dts/dtb.img.0.dts`, `dtbo.img.0.dts` and `dtbo.img.1.dts`: the stock y2s device trees.
- Samsung's `drivers/clk/samsung/clk-exynos9830.c`, `drivers/pci/controller/dwc/pcie-exynos-host-v0.c`, `drivers/gpu/drm/panel/panel-samsung-s6e3hab.c` and the `sec_ts`, `s2dos05` and `max77705` drivers.
- Project Silicium's `S5E9830Pkg`: memory map, GPIO banks, HSI2C and UFS.
- z3s's notes and patches for the same SoC.
- Measurements in the `fb:`, `wifi:`, `bt:`, `snd:`, `mali:`, `dvfs:` and `ufs:` log lines.
