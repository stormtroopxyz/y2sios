# 03 — Battery, charger, USB

## Status (2026‑10‑24): battery DONE

`bsd/dev/redwood_max77705.c` (`bms=1`, now in `y2s.sh`'s default cmdline) reads the MAX77705
fuel gauge (0x36) and charger (0x69) every 5 s, bit-banged on gpp1‑6/7, **read only**. It
reports through `redwood_battery_report()` → `RedwoodBattery` (IOPMPowerSource). On the phone:
the status bar % matches Android, the charging bolt follows plug/unplug, and the log reads
`100.00 % 4331 mV 45 mA, 32.09 C, 3797/3797 mAh, full, input 5107 mV 496 mA`.

Corrections to the facts below, from Samsung's driver: the current LSB at `fg_resistor = 2` is
**312.5 µA**, and capacity is **1 mAh** per LSB. The gauge's CYCLES register is zeroed by Samsung's
driver at every boot (the real count lives in EFS), so it is not reported. Voltages need 64‑bit
math (`raw × 78125` overflows 32 bits above ~4.29 V).

Open: U4 (real battery temperature), U5 (USB device mode), the adapter type (MUIC at 0x25).

## What already exists (upstream, Qualcomm)

Data flow on the upstream phone:
`(loader shared memory | ADSP) → redwood_battery_report(struct redwood_battery_state) →
RedwoodBattery (IOPMPowerSource subclass, iokit/Kernel/RedwoodBattery.cpp) → powerd /
IOPS → status bar, lock screen, Settings › Battery`.

- `bsd/sys/redwood_input.h`: `struct redwood_battery_state` (percent in hundredths, µV,
  µA, temperature, present, external, Linux `status`, µAh charge/full/design, cycles,
  USB type, voltage, and current).
- `RedwoodBattery.cpp` already publishes the properties iOS reads
  (`CurrentCapacity`, `IsCharging`, `ExternalConnected`, `AdapterDetails`, …).
- `userland/src/battery_test.c` + `rc.jobs.battery` exercise it.

**So a y2s battery driver only has to fill `struct redwood_battery_state` and call
`redwood_battery_report()`.** The iOS side already works.

## Hardware (verified)

**MAX77705** (PMIC + charger + fuel gauge + USB‑C/PD), `max77705@66`:

| | |
|---|---|
| Bus | **hsi2c_7** (`0x105C0000`), pins **gpp1‑6 SCL / gpp1‑7 SDA**, PERIC0 pinctrl `0x10430000` + `0x20`. This is the same bank as the S2DOS05 bus (gpp1‑0/1) |
| Interrupt | `gpa1‑6` (ALIVE) |
| I2C addresses (`drivers/mfd/max77705.c`) | PMIC `0x66`, USB‑C/MUIC `0x25`, **charger `0x69`**, **fuel gauge `0x36`**, debug `0x62` |
| Battery | 4500 mAh (`battery,battery_full_capacity = 0x1194`), `fuelgauge,fg_resistor = 2` (2 mΩ sense) |

Fuel gauge (16‑bit little‑endian registers, `max77705_fuelgauge.c`):

| Reg | Name | Scale |
|---|---|---|
| `0x06` | SOCREP | 1/256 % |
| `0x05` | REMCAP_REP | capacity LSB = 5 µVh / R_sense → 2.5 mAh at 2 mΩ (**verify against `FULLCAP_REP`**) |
| `0x35` | FULLCAP_REP | same unit as above |
| `0x09` | VCELL | 78.125 µV |
| `0x0A` / `0x0B` | CURRENT / AVG_CURRENT | signed; 1.5625 µV / R_sense = **781.25 µA** at 2 mΩ |
| `0x17` | CYCLES | |
| `0x18` | DESIGNCAP | |

Samsung shows the UI percentage as `RepSOC` rescaled by `capacity_max` (1000 =
100.0 %, margin 300) in its battery manager. The first version can use raw `RepSOC`
and match the scaling later.

Charger (`0x69`):

| Reg | Name | Bits |
|---|---|---|
| `0xB2` | CHG_INT_OK | bit6 `CHGIN_OK` (USB power present), bit5 `WCIN_OK` (wireless), bit2 `BATP_OK` |
| `0xB3` | DETAILS_00 | `CHGIN_DTLS` bits 6:5 |
| `0xB4` | DETAILS_01 | `CHG_DTLS` bits 3:0 (charge state), `BAT_DTLS` bits 6:4 |

**Read only.** The MAX77705 charges by itself with the settings sboot left. We never
write charger configuration (`CNFG_*` from `0xB7`). A wrong write could stop charging or
over-charge.

## Steps

### U1 — Share the bit‑bang I2C

Move the open‑drain bit‑bang bus out of `redwood_sec_ts.c` into
`bsd/dev/redwood_bbi2c.c` + `bsd/sys/redwood_bbi2c.h` (bus struct, setup, restore,
`xfer`). Touch, the S2DOS05 and the MAX77705 all use it. **gpp1 is shared** by the S2DOS05
(pins 0/1) and the MAX77705 (pins 6/7). The CON/DAT read‑modify‑writes on that bank
must go under one lock, so the driver modules need a per‑bank spin lock in the shared module.

### U2 — `bsd/dev/redwood_max77705.c` (fuel gauge + charger state)

- Boot arg `battery=2` (`battery=1` means the Qualcomm loader path).
- Every 5 s, read SOCREP, VCELL, CURRENT, AVG_CURRENT, REMCAP_REP, FULLCAP_REP,
  CYCLES, CHG_INT_OK, DETAILS_00/01.
- Fill `redwood_battery_state`:
  - `external` = `CHGIN_OK|WCIN_OK`
  - `status`: charging when `CHG_DTLS` is a charge state, full when it is done, not charging otherwise
  - `usb_type` from `CHGIN_DTLS`
- Then call `redwood_battery_report()`.
- Log one line per minute: `battery: 73.4 % 3.912 V -412 mA, usb present, charging`.

### U3 — Verify

The status bar shows the percentage, plugging in shows the lightning bolt, and Settings ›
Battery shows level history. Compare the numbers with Android's
`/sys/class/power_supply/battery/*` at the same moment.

### U4 — Temperature

Samsung measures battery temperature with a thermistor on the AP's ADC, not the fuel gauge.
Start with the fuel gauge `TEMPERATURE_REG 0x08` (if Samsung enables it) or report
a fixed 25 °C. Find the ADC channel in `battery,*` DT properties later.

### U5 — USB device mode (long term)

- DWC3 at `0x10E00000` (`dr_mode = "otg"`), USB DRD PHY at `0x10C00000` (+
  `0x10AE0000`, `0x10AF0000`), compatible `samsung,exynos-usbdrd-phy`.
- Mu‑Silicium has `SamsungPkg/Drivers/UsbfnDwc3Dxe` (device mode), not yet wired
  into `y2sPkg`. The cheapest first win is **USB in the loader** (fastboot‑style upload
  of kernels, so iteration needs no Android): add `UsbfnDwc3Dxe` to `y2sPkg` and teach
  the loader's USB code the `EFI_USB_FUNCTION_IO_PROTOCOL` (it looks for Qualcomm's GUID today).
- In XNU: a DWC3 gadget driver (CDC‑NCM networking or a serial console) is a large
  project. It would give SSH/log streaming. Defer until performance and battery are done.

### U6 — Charging policy (only if needed)

If sboot's defaults charge too slowly (500 mA), raise the input current limit
(`CNFG_09`/`CNFG_10`) only after reading Samsung's charger driver end to end and
matching its values for the detected adapter type. Not before U3 works.
