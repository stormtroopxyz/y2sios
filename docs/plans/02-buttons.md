# 02 — Power and volume buttons

## Hardware (verified)

From `dtbo.img` (`gpio_keys`, compatible `gpio-keys`) and the UEFI's working
`y2sPkg/Library/KeypadDeviceLib/KeypadDeviceLib.c`:

| Key | Pin | Linux code | Notes |
|---|---|---|---|
| Volume up | **gpa0‑3** | `KEY_VOLUMEUP` (115) | |
| Volume down | **gpa0‑4** | `KEY_VOLUMEDOWN` (114) | |
| Power | **gpa2‑4** | `KEY_POWER` (116) | `gpio-key,wakeup` |

- All three are **plain GPIOs, active‑low** (UEFI passes `!IsPressed`). The pinctrl function is
  `0xF` (EINT), with no pull‑up or pull‑down (the board has pull‑ups).
- Bank A lives in the **ALIVE pinctrl at `0x15850000`**: `gpa0` at `+0x00`, `gpa2` at
  `+0x40` (UEFI `GpioBankLib`, controller index 5 in `S5E9830Pkg.dsc.inc`'s list
  `0x10430000, 0x10730000, 0x13040000, 0x13C30000, 0x15580000, 0x15850000, 0x15C30000`).
  `DAT` (+0x04) reads the pin level whatever the function, so no reconfiguration is needed.
- The power key is **not** read through the S2MPS19 PMIC (which sits behind
  SPEEDY/ACPM). The GPIO is enough.
- **Hardware reset:** holding Vol‑Down + Power for ~7 s or more is the PMIC's manual reset. That
  stays as the last‑resort escape whatever the driver does.

## Existing software

- `bsd/dev/redwood_buttons.c` is Qualcomm‑only (PMK8350 PON/GPIO over SPMI). It has
  the logic we want: two‑sample debounce, "believe a key only after it has been seen
  released", and logging. It posts `redwood_touch_post(REDWOOD_BUTTON_DOWN/UP, REDWOOD_BUTTON_*)`.
- Userland `inject.c` (backboardd) turns those into HID consumer events: lock →
  Power, volume → Volume Up/Down, with `notify` state `com.redwood.button.lock`. So
  **only the kernel side is missing**.

## Steps

### B1 — `bsd/dev/redwood_exynos_keys.c`

- Boot arg `keys=1` (or follow `touch_ts` like the Qualcomm driver follows
  `touchscreen`). `keys=0` disables it.
- `ml_io_map(0x15850000, 0x1000)`. Each key is (bank offset, pin, record).
- Kernel thread, poll every 16 ms. Same debounce as `redwood_buttons.c`
  (two equal samples; ignore a key until seen released).
- Post DOWN/UP through `redwood_touch_post`.
- Register in `bsd/conf/files` and call it from `redwood_input_init()` next to
  `redwood_sec_ts_init()`.

### B2 — Verify on the phone

Log lines `keys: power PRESSED/released`. In iOS: power locks/wakes (the backlight
behaviour depends on B4), volume shows the HUD.

### B3 — Escape hatch: Vol‑Down + Power held for 3 s → `loader_return()`

This hands control back to the loader, which saves the log (see 00‑F3). It fires before
the PMIC's ~7 s hardware reset, so it is a clean way to end a run, and if it fails the
hardware reset still happens. Boot arg `keys_escape=0` disables it.

### B4 — Real screen‑off on lock (later)

iOS asks the framebuffer for power changes (`kFBMethodRequestPowerChange`, selector 12
in `MiniIOMobileFramebuffer.cpp`). Turning the AMOLED panel off needs DSI commands
through DECON, or at least backlight control (`s6e3hab` panel driver in Samsung's
kernel). Until then, a minimal version: on display‑off, **stop swapping and fill
black**. On OLED, black pixels are off, which saves power and shows the lock state.
