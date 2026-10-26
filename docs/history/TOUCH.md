# Touch driver for y2s (Galaxy S20+, Exynos 990)

Status: **WORKING on the phone** (SpringBoard responds to touch, `touch_ts=1`).

## What fixed it (read this first; the history below it is superseded)

Three wrong assumptions were blocking touch:

1. **The controller is not STM FingerTipS.** `compatible = "sec,sec_ts"` plus
   firmware `tsp_sec/y79a_y2.bin` is Samsung System LSI's own touch IC (S6SY79A).
   The stock kernel runs `drivers/input/touchscreen/sec_ts/y79a` (Samsung OSRC,
   e.g. `pascua28/android_kernel_samsung_exynos990`). The `stmfts.c` commands
   (`0x80`/`0x86`/`0x91`…) are wrong for this chip. The events are 16 bytes, not 8.
2. **The rails were off, and the PMIC is on a different bus.** `tsp_io`/`tsp_avdd`
   are S2DOS05 LDO2/LDO3, and the S2DOS05 (`0x60`) sits on **hsi2c_4 = gpp1‑0/1**
   (PERIC0 pinctrl `0x10430000`, bank `+0x20`), not on the touch bus. Bit-banging
   that bus needs no CMU clock, so the "circular dependency" did not exist.
3. **The USI register was the wrong one.** `0x108D0000`'s USI switch is
   `0x10721024` (SYSREG_PERIC1, per LineageOS `exynos9830-usi.dtsi`, also in
   `S5E9830Pkg/Library/UsiLib/Usi.c`), not `0x10421050`. This no longer matters
   because the driver bit-bangs.

### Power-up (S2DOS05, Samsung `drivers/regulator/s2dos05.c`)

| Reg | Name | Use |
|---|---|---|
| `0x00` | DEV_ID | |
| `0x03` | EN | bit1 LDO2 (`tsp_io`), bit2 LDO3 (`tsp_avdd`); the other bits feed the display, so they are never cleared |
| `0x05` | LDO2_CFG | VSEL bits 6:0, 1.5 V + n×25 mV → 1.8 V = `0x0C` |
| `0x06` | LDO3_CFG | VSEL bits 6:0, 2.7 V + n×25 mV → 3.0 V = `0x0C` |
| `0x61` | DEVICE_ID_PGM | |

Sequence (`sec_ts_power()`): set the VSEL values only while a rail is off. Then
`EN |= LDO2`, wait 1 ms, `EN |= LDO3`, wait 100 ms.

### sec_ts protocol (y79a `sec_ts.h`)

Each read is: write the command byte, repeated start, then read N bytes. Each write is the command byte followed by its data.

| Cmd | Meaning |
|---|---|
| `0x10` | SENSE_ON |
| `0x21` | firmware integrity (bit7 = OK) |
| `0x22` | device ID (5 bytes) |
| `0x23` | panel info (11 bytes: max_x BE16, max_y BE16, display x/y, tx, rx) |
| `0x30` | set touch function (u16 LE, default `0x0061` = touch, palm and wet) |
| `0x52` | ID (3) · `0x55` boot status (`0x10` bootloader, `0x20` app) · `0xA3` firmware version (4) · `0xAF` TS status (4) |
| `0x60` | read one event (16 B) · `0x61` read N more · `0x62` clear stack |

Event byte 0: `eid = b0 & 3` (0 coordinate, 1 status, 3 invalid),
`tid/stype = (b0 >> 2) & 0xF`, `action = b0 >> 6` (1 press, 2 move, 3 release).
Coordinates: `x = b1<<4 | b3>>4`, `y = b2<<4 | b3&0xF`, finger = `tid − 1`,
`ttype = (b6>>6)<<2 | b7>>6`, events left = `b7 & 0x1F`. Boot complete is a status
event with `stype` 2 (INFO) and `b1 == 0x00`. If `b2 == 0x20` as well, the
controller had a watchdog reset, so the driver sends SENSE_ON again.
The attention line `gpa1-0` (ALIVE pinctrl `0x15850000` +`0x20`) is low while events are queued, so the
poll loop only touches the bus when it is low.

### Boot args

`touch_ts=1` probe + poll · `touch_ts=2` probe, then panic with a one-line summary
(diagnostics) · `touch_ts=3` probe only · `touch_pwr=0/1/2` (skip / enable /
power-cycle the rails) · `touch_attn=0` (ignore the attention pin) ·
`touch_pullup=1` · `touch_poll_us` · `touch_flip` · `touch_trace`.

---

## History: where the bring-up stood before the fix (superseded)

The kernel log (via a deliberate panic, so the loader saves and prints it) shows:

```
sec_ts: HSI2C_15 at 0x108d0000, as found: CTL 0x8 CONF 0x980110ff FS3 0xff0000
sec_ts: USI_15 was 0x4, now 0x4 (I2C)
sec_ts: CMU PERIC1 bus 0x200000 -> 0x300000, ip 0x200000 -> 0x300000
sec_ts: scratch: trail 0xffffff->0xffffff addr 0xff0fffff->0xff0fffff timeout 0x800000ff->0x800000ff
panic: sec_ts: HSI2C_15 ignores every write ... the block is not running
```

Three registers were written and read back; all three still hold their **reset
defaults** (`0xffffff`, `0xff0fffff`, `0x800000ff`). Reads answer, writes are
dropped — the controller is held in reset with its **IP clock gated**. Nobody
brought it up: not the bootloader (those are reset values), not us yet. Android's
kernel does it at probe time.

What has been **ruled out**:
- USI routing: `0x10421050` is already `0x4` (I2C); writing it is a no-op.
- CMU_TOP's PERIC1 domain gates: `0x20d4`/`0x20d8` bits 20|21 go from `0x200000`
  to `0x300000` and the writes stick (so CMU_TOP itself is alive and mapped).
- Register map/offsets: they match Samsung's own `EFI_HSI2C_BUS` struct
  (`Mu-Silicium/Silicon/Samsung/SamsungPkg/Drivers/HsI2cDxe/HsI2c.h`): ctrl 0x00,
  int_enable 0x20, fifo_status 0x30, tx/rx 0x34/0x38, conf 0x40, auto_conf 0x44,
  timeout 0x48, trans_status 0x50, timing 0x54..0x6c, address 0x70, **usi_con 0xC4**.
- Pin mux: the UEFI's `HsI2cLib` confirms hsi2c_15 = **gpp5-6 (SCL) / gpp5-7 (SDA)**,
  function 3, 200 MHz, fast mode — identical to the DT.

## Round 2: bit-bang works electrically, but the bus is dead

I replaced the HSI2C transport with I2C bit-banged on gpp5-6/7 (pinctrl PERIC1
`0x10730000`, GPP5 at offset 0, CON/DAT/PUD at `+0x00/+0x04/+0x08`, function 0 =
input, 1 = output, pull-up = 3). The pins work:

```
sec_ts: after setup: CON 0x11000000 DAT 0xe3 PUD 0x1101f111 (both lines high)
sec_ts: SDA as input reads 0 (1 = high)
sec_ts: ghost address 0x55 acked=1 (must be 0)
```

Driving the lines as outputs works (DAT shows both high), but as soon as SDA is an
input it reads **0**, and an address nobody owns "ACKs". Both say the same thing:
**SDA is held low**, so every ACK was fiction and every byte read as `0x00`
(`info 00 00 00 00 00 00 00 00`, even after SLEEP_OUT + SYSTEM_RESET).

Note that at boot the pins already read low as inputs (`CON 0 DAT 0x23`), i.e. the
lines were low before anything of ours ran.

### What that means

The touch controller is not powered / its bus is clamped low. Its rails come from
an **S2DOS05** PMIC (overlay):

| Regulator | Node | Voltage |
|---|---|---|
| `tsp_io` | `dp_ldo2` (s2dos05-ldo2) | 1.8 V |
| `tsp_avdd` | `dp_ldo3` (s2dos05-ldo3) | 3.0 V |

Both are `regulator-boot-on`, so the bootloader is expected to leave them on — but
nothing in our boot path enables or checks them, and XNU has no PMIC driver for
this phone.

**This is a circular dependency:** to power the touch we must talk to the S2DOS05
(at I2C `0x60`, on its own HSI2C bus), which needs a working I2C controller —
which is exactly the block whose clock we could not find (CMU_PERIC1).

### So the real critical path is one item long

1. **Get one HSI2C controller running**: find `cmu_peric1`'s base + the per-IP
   `hsi2c` gate bit (mainline has `samsung,exynos990-cmu-peric1`; the phone's
   downstream DT only exposes CMU_TOP at `0x1a330000`). With that, the HSI2C
   scratch test (`trail 0xffffff` must become `0x0badf00d`) passes.
2. Then power `tsp_io`/`tsp_avdd` through the S2DOS05.
3. Then the FTS probe answers and the bit-bang (or HSI2C) transport can read
   events; the rest of the driver is already written.


Exynos 990 has a **separate `cmu_peric1` clock controller** (mainline:
`compatible = "samsung,exynos990-cmu-peric1"`, added with PERIC0/1 USI/HSI2C
support). The per-IP `gate_hsi2c_clk` lives there, not in CMU_TOP. The phone's
downstream DT only exposes CMU_TOP (`clock-controller@0x1a330000`), so the base
address of CMU_PERIC1 still has to be found (candidates: a block near the PERIC1
sysreg `0x10720000`, or a region the downstream driver reaches through its own
table). **This is the one missing fact.**

## Two ways forward

1. **Find CMU_PERIC1's base + the hsi2c gate bit** (mainline `clk-exynos990.c` for
   peric1, or Samsung's downstream clock driver). Then it is a three-line change:
   map the block, set the gate's bit 20|21, and the scratch test should pass.
2. **Bit-bang I2C over the GPIOs** — no clocks involved, because the pinctrl block
   is always on. Everything needed is known:
   - GPP5 bank at `0x10730000 + 0x00` (UEFI `GpioBankLib`: PERIC1, BANK_ID_P,
     Number 5, offset 0x0), pins 6 = SCL, 7 = SDA.
   - Per-bank registers: CON `+0x00` (4 bits/pin, so pin6 = bits 27:24, pin7 =
     bits 31:28), DAT `+0x04`, PUD `+0x08`, DRV `+0x0C`.
   - Function 0 = input, 1 = output; drive SCL push-pull, SDA out for sending and
     in for reading, ~50–100 kHz. That is plenty: a touch report is ~10 bytes.

Option 2 is self-contained and cannot be blocked by a clock we cannot find, so it
is the safer next step; option 1 is cleaner if the register shows up.

## Debug loop (this is what makes iteration possible)

- Rebuild: `export GIC_V2=1` (**must be exported** — `./xnu-build.sh` is a child
  process; sourcing `y2s.conf` without `export` silently builds a GICv3 kernel,
  which dies right after `pe_arm_init_debug`).
- Ship: copy `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` to
  `out/mach.y2s`, rebuild `out/y2s-recovery.img` (UEFI at 0, kernel at 32 MB,
  `root.apfs` at 48 MB), `adb push` + `dd` to `/dev/block/by-name/recovery`,
  `adb reboot recovery`.
- Read the log: the kernel log only reaches storage when the kernel *returns*, so
  the driver **panics on purpose** with the numbers in the panic message. The
  panic is the last thing in the log, which is what the loader prints on screen.
  Reliable read-back: force off → Download Mode → flash stock `misc` → Android →
  `dd` `recovery` @16 MB → pull.
- `loader_return_after=N` does **not** fire on this phone (tested: 25 s, no return),
  so it cannot be used as the log trigger.

## Hardware facts (extracted from the phone's own dtbo.img / dtb.img)

| What | Value |
|---|---|
| Controller | **STMicroelectronics FingerTipS** (Samsung `sec_ts`, firmware `y79a_y2`) |
| I2C address | `0x48` |
| Bus | **`hsi2c_15` @ `0x108D0000`**, pins **gpp5-6/7**, function 3 |
| USI mode | `0x10421050` = `0x4` (already set) |
| Attention pin | **`gpa1-0`**, falling edge (`sec,irq_type = 0x2008`) — polling instead, so unused |
| Coordinates | `sec,max_coords = <0x1000 0x1000>` → 4096×4096 |
| Supplies | `tsp_avdd`/`tsp_io`, `sec,regulator_boot_on` (bootloader leaves them on) |

## ST FingerTipS protocol (mainline `stmfts.c`)

Command-then-read: `0x86` read-all-events (up to 32×8 bytes), `0x85` one event,
`0x80` info block (`[6..7]` = chip id, BE), `0x91` sleep-out, `0x93` MT sense on,
`0xa1` clear stack. Event = 8 bytes:

```
slot = (e[0] & 0xF0) >> 4
x    = e[1] | ((e[2] & 0x0F) << 8)      /* 0..4095 */
y    = (e[2] >> 4) | (e[3] << 4)
```
ids: `0x03` enter, `0x04` leave, `0x05` motion, `0x10` ready, `0x16` status.

## Injection path (already in the kernel)

`redwood_touch_post(type, slot, x, y)` with x/y in 0..65535 (scale by 65535/4095,
`touch_flip` mirrors). `redwood_input.c` queues it for the window server and
implements the "five fingers held 1.5 s → back to the loader" escape.

## GPU: not feasible on this hardware

`redwood_adreno.c` is Qualcomm-specific (Adreno 642L, GMU, RPMh, SMMU). The S20+
has a **Mali-G77 MP11**, and iOS draws with **Metal** — a proprietary driver ABI
with no public interface and no reference driver for Mali. The system already
falls back to software rendering (which is why the home screen appears), and that
is the realistic end state. Wallpaper and widget blank-ness follow from the same
two gaps: no GPU compositing, and no network for `weatherd` (widgets also render
through a GPU-backed render server).


## Hardware facts (extracted from the phone's own `dtbo.img` / `dtb.img`)

| What | Value |
|---|---|
| Controller | **STMicroelectronics FingerTipS** (Samsung `sec_ts` driver, FTS strings in the kernel) |
| I2C address | `0x48` |
| I2C bus | **`hsi2c_15` @ `0x108D0000`** (size `0x1000`) |
| USI mode register | **`0x10421050`** (USI v2) — write `0x4` (`USI_V2_SW_CONF_I2C`) to route the block to I2C |
| Bus pins | **`gpp5-6` (SCL), `gpp5-7` (SDA)** — pin function `3` (I2C), pinctrl block `0x10730000` |
| Attention / IRQ pin | **`gpa1-0`** — pin function `0x0f` (EXT_INT), falling edge (`sec,irq_type = 0x2008`), pinctrl block `0x10430000` |
| Coordinates | `sec,max_coords = <0x1000 0x1000>` → **4096 x 4096** (scale to the 1440x3200 panel) |
| Supplies | `tsp_avdd`, `tsp_io` — **`sec,regulator_boot_on`**: the bootloader leaves them on |
| Firmware | `tsp_sec/y79a_y2.bin` (no firmware update needed if the controller is already running) |
| Panel | 1440x3200 (`F1000000`) |

`hsi2c_15` is inside `PERIC1` (`0x10720000`..`0x10730000`) and `0x10421050` is inside
`PERIC0` (`0x10420000`); both ranges are already in the UEFI memory map, and XNU's
redwood drivers reach device registers with `ml_io_map()`.

## ST FingerTipS protocol (from mainline `drivers/input/touchscreen/stmfts.c`)

I2C is command-then-read: write one command byte, then read N bytes.

| Command | Value | Meaning |
|---|---|---|
| `STMFTS_READ_ALL_EVENT` | `0x86` | read the whole event stack (up to 32 events) |
| `STMFTS_READ_ONE_EVENT` | `0x85` | read one event (8 bytes) |
| `STMFTS_READ_INFO` | `0x80` | read the info block |
| `STMFTS_SYSTEM_RESET` | `0xA0` | reset the controller |
| `STMFTS_SLEEP_OUT` | `0x91` | wake |
| `STMFTS_MS_MT_SENSE_ON` | `0x93` | enable multi-touch sensing |
| `STMFTS_CLEAR_EVENT_STACK` | `0xA1` | drop pending events |

Event = **8 bytes**:

| Byte | Meaning |
|---|---|
| 0 | low nibble: event id; high nibble: finger slot |
| 1 | X bits 7..0 |
| 2 | low nibble: X bits 11..8; high nibble: Y bits 3..0 |
| 3 | Y bits 11..7 |
| 4 | touch major |
| 5 | touch minor |
| 6 | orientation |
| 7 | area / pressure |

```
slot = (e[0] & 0xF0) >> 4
x    = e[1] | ((e[2] & 0x0F) << 8)      /* 0..4095 */
y    = (e[2] >> 4) | (e[3] << 4)        /* 0..4095 */
```

Event ids: `0x02` MT detected, `0x03` MT enter (down), `0x04` MT leave (up),
`0x05` MT motion (move), `0x0E` key, `0x0F` error, `0x10` controller ready,
`0x16` status.

## Exynos HSI2C register map (from mainline `i2c-exynos5.c`, the Exynos8895+/990 layout)

| Offset | Register |
|---|---|
| `0x00` | `HSI2C_CTL` (`FUNC_MODE_I2C` bit0, `MASTER` bit3, `RXCHON` bit6, `TXCHON` bit7, `SW_RST` bit31) |
| `0x04` | `HSI2C_FIFO_CTL` |
| `0x08` | `HSI2C_TRAILIG_CTL` |
| `0x0C` | `HSI2C_CLK_CTL` |
| `0x20` | `HSI2C_INT_ENABLE` |
| `0x24` | `HSI2C_INT_STATUS` |
| `0x2C` | `HSI2C_ERR_STATUS` |
| `0x30` | `HSI2C_FIFO_STATUS` |
| `0x34` | `HSI2C_TX_DATA` |
| `0x38` | `HSI2C_RX_DATA` |
| `0x40` | `HSI2C_CONF` (`AUTO_MODE` bit31, `10BIT_ADDR` bit30, `HS_MODE` bit29) |
| `0x44` | `HSI2C_AUTO_CONF` |
| `0x48` | `HSI2C_TIMEOUT` |
| `0x4C` | `HSI2C_MANUAL_CMD` |
| `0x50` | `HSI2C_TRANS_STATUS` |
| `0x54`..`0x6C` | `TIMING_HS1..SLA` (bus timing; `default-clk` is 200 MHz) |
| `0x70` | `HSI2C_ADDR` |

Status bits: `TRANS_DONE` bit7, `TRANS_ABORT` bit8, `NO_DEV_ACK` bit9, `NO_DEV` bit10,
`TIMEOUT` bit11.

## Bring-up sequence (first milestone)

1. `ml_io_map(0x10421050)` → write `0x4` (USI_15 → I2C).
2. `ml_io_map(0x108D0000)`:
   - software reset (`HSI2C_CTL = SW_RST`), poll until clear;
   - `HSI2C_CTL = FUNC_MODE_I2C | MASTER`;
   - program the timing registers for 400 kHz from a 200 MHz source clock;
   - `HSI2C_CONF = 0` (manual mode), `HSI2C_TIMEOUT` = a sane value.
3. **Test: read the FTS device id.** Write `0x80` (`READ_INFO`), read back a block, print
   it. If the chip answers, the clock, pins, USI routing and bus timing are all correct.
   This is the single most informative test and it needs no IRQ.
4. Then poll `READ_ALL_EVENT` (`0x86`) at ~100 Hz (no interrupt wiring needed at first),
   decode events, and call `redwood_touch_post(type, slot, x_scaled, y_scaled)`.
   `redwood_input.c` already queues touches for the window server and already implements
   the "5 fingers held → back to the loader" escape.

Gate everything behind a boot arg (`touch=1`), the way `gpu=1` and `virtio_input=1` work,
so a failure costs one boot and says which step failed.

## Open questions / risks

1. **HSI2C gate clock.** The CMU gate for `hsi2c_15` must be on. Samsung's bootloader often
   leaves PERIC clocks running; if the first register read returns 0 or hangs, the clock is
   off and we need the CMU register (CMU_PERIC1 @ `0x10720000` or CMU_HSI1 @ `0x13000000`)
   and its gate bit. Read-before-write, one line of output per step.
2. **Reset.** The DT node has no reset GPIO; the controller is presumably kept alive by the
   bootloader (it is powered for the boot logo). If it does not answer, try
   `STMFTS_SYSTEM_RESET` (`0xA0`) and `SLEEP_OUT` (`0x91`) first.
3. **Scaling.** FTS reports 0..4095 in both axes; the panel is 1440x3200, and iOS renders
   1125x2436 (D22/iPhone X) in software. Scale into the same space `redwood_virtio_input`
   uses so SpringBoard's hit testing matches what is drawn.
4. **Rotation.** `redwood_virtio_input.c` already maps QEMU coordinates to the panel; reuse
   that mapping.

## GPU: not feasible on this hardware (be clear about it)

The project's GPU driver (`bsd/dev/redwood_adreno.c`, 4125 lines) is entirely
Qualcomm-specific: Adreno 642L, GMU, RPMh, GENI, SMMU. The S20+ has a **Mali-G77 MP11**.

iOS draws with **Metal**, which is Apple's proprietary user/kernel driver ABI. Supporting it
on Mali would mean writing a Metal driver for a GPU Apple never shipped — there is no public
ABI, no reference driver, and no upstream work to build on. The system already falls back to
software rendering (which is exactly why the home screen appears), and that is the realistic
end state for this port. Anything GPU-shaped here would be a software rasteriser
improvement, not a Mali driver.

## Where the work goes

- New file: `bsd/dev/redwood_sec_ts.c` (HSI2C + FTS + injection), modelled on the structure
  of `redwood_goodix.c` but with Exynos register access.
- Hook: `redwood_input_init()` (`bsd/dev/redwood_input.c:209`) next to
  `redwood_virtio_input_init()`.
- Boot arg: `touch=1`.
- Test loop: rebuild the kernel, `dd` it to `recovery` @ 32 MB from Android, `adb reboot
  recovery`, read the kernel log from `recovery` @ 16 MB (or off the screen — the loader now
  clears the screen and prints the kernel log there).
