# Handoff — HackiOS on the Samsung Galaxy S20+ (y2s, Exynos 990)

Written at the end of the session in which **iOS 16.5 first booted on the phone**.
Everything below was measured on the real device unless it says otherwise.

---

## 1. TL;DR

| Thing | State |
|---|---|
| XNU kernel built natively on Apple Silicon macOS | **works** |
| iOS 16.5 userland built (root + system images) | **works** |
| QEMU baseline (boots to SpringBoard/lock screen) | **works** |
| `y2sPkg` (Mu‑Silicium UEFI for this phone) | **boots on the phone** |
| `xnuloader` (our XNU loader) | **runs on the phone** |
| XNU kernel on the phone | **works** — BSD init, mounts APFS root |
| **iOS userland on the phone** | **works** — Apple logo → SpringBoard home screen |
| Touch | **works** — S2DOS05 rails enabled + Samsung `sec_ts` protocol, both buses bit-banged (see §10.0) |
| GPU (Metal) | **not feasible** on Mali‑G77; software rendering is the end state |
| Wi‑Fi / audio / camera / storage | not started |

The phone currently runs **Android** (PixelOS 16, rooted). Nothing is broken or in
need of recovery.

---

## 2. Goal and constraints

Port HackiOS (open‑source XNU kernel + unmodified iOS 16.5 userland) to the Galaxy
S20+ 5G so iOS boots on the physical phone.

Constraints that shaped everything:

- Build **natively on Apple Silicon macOS**, not on x86 Linux.
- Flashing the stock ROM is acceptable; the user's Android data does not need to
  survive.
- Prefer `adb` over button combos.
- The user cannot reliably use key combos, so the flash loop is designed to avoid
  them where possible.

---

## 3. Hardware

| | |
|---|---|
| Phone | SM‑G986B, codename **y2s**, Galaxy S20+ 5G |
| SoC | Samsung **Exynos 990** (S5E9830), 8 cores |
| CPU | Cortex‑A76, `MIDR_EL1 = 0x412FD050`; MPIDRs `{0,1,2,3,4,5,0x100,0x101}` |
| RAM | 12 GB |
| Display | 1440×3200, framebuffer at **`0xF1000000`**, row bytes 5760 |
| Interrupt controller | **GIC‑400 (GICv2)**, distributor `0x10101000`, CPU interface `0x10102000`, `IIDR 0x0202143b` |
| Arch timer | `CNTFRQ_EL0` reads **0** (firmware never set it; assumed 26 MHz) |
| Storage | UFS |
| Bootloader | unlocked, October 2026 |

Partitions (measured with `blockdev --getsize64`):

| Partition | Size |
|---|---|
| `super` | 9,437,184,000 (8.79 GiB) |
| `userdata` | 116,253,523,968 (108 GiB) |
| `recovery` | 69,009,408 (65.8 MiB) |
| `boot` | 61,865,984 |
| `cache` | 629,145,600 |
| `prism` | 1,048,576,000 |

`super`'s logical partitions (`system`, `vendor`, `product`, `odm`, `system_ext`)
end at sector 12,735,927, leaving **~2.7 GiB of unused tail** — this is where the
iOS root image lives (see §5).

---

## 4. The boot chain, end to end

```
sboot (Samsung bootloader)
  └─ loads the `recovery` partition as an Android boot image
       ├─ BootShim (112 bytes, from the prebuilt Mu-x1s.img)
       │    copies the 2 MB UEFI FD from the kernel payload to 0xE8000000
       │    and jumps to it
       ├─ Mu-Silicium UEFI (our y2sPkg FD, 2 MB)
       │    BDS finds nothing to boot → PlatformBootManagerUnableToBoot()
       │    → XnuLoaderMain()  (our hook in BootManagerLib)
       │      → StartPayloadLoader(): finds "loader.efi" in the XNUP payload
       │        at 0xE8100000 (inside the FD's unused tail) and StartImage()s it
       └─ xnuloader (`loader.efi`, 69,120 bytes, loaded at 0xA1000000)
            reads the XNU kernel from `recovery`+32 MB
            reads root.apfs (md1) from `recovery`+48 MB
            reads system.apfs (md0, 2.32 GB) from `super`+0x184B00000
            builds a device tree + boot_args, maps a physical window,
            exits boot services, MMU off, jumps to XNU
```

### `recovery` partition layout (the "disk")

| Offset | Size | Contents |
|---|---|---|
| 0 | 2,445,312 | Android boot image: BootShim + UEFI FD + XNUP payload |
| 16 MiB (`0x01000000`) | 2 MiB | kernel log, written by the loader |
| 32 MiB (`0x02000000`) | 15.2 MiB | XNU kernel (`mach.y2s`, Mach‑O arm64) |
| 48 MiB (`0x03000000`) | 4 MiB | `root.apfs` — the `/redwood` disk (md1) |

And in `super`:

| Offset | Size | Contents |
|---|---|---|
| `0x184B00000` (6,521,094,144) | 2,492,203,008 | `system.apfs` — the iOS root (md0) |

Why this layout:

- The whole thing is written **from Android with `dd`**, so iterating never needs
  Download Mode.
- `system.apfs` is 2.32 GB and the only partitions that big are `super` and
  `userdata`. `super`'s tail is genuinely unused, so Android keeps working.
- The kernel sits in `recovery` so a kernel-only change is one `dd` at 32 MiB.

---

## 5. The five bugs that actually cost the time

### 5.1 Removing ACPI from `y2sPkg` black‑screens the phone

`y2sPkg` was copied from `x1sPkg` and the ACPI tables were removed ("our loader
builds its own device tree"). That is wrong: **`BdsDxe`'s dependency expression
requires the ACPI table protocol**, so without `AcpiTableDxe` the BDS never
dispatches — no console, no logo, then a reset that the bootloader turns into
Download Mode. It looked exactly like a kernel crash.

**Fix:** keep ACPI. `y2s.dsc`/`y2s.fdf` use `x1s/AcpiTables.inf` from the
`Silicium-ACPI` submodule (same S5E9830 SoC). `UpdateAcpiTables()` itself is a null
library, so the tables are inert but the protocol exists.

### 5.2 Appending the payload after the FD is rejected by sboot

The prebuilt `Mu-x1s.img` is `shim(112) + FD(2 MB)`, and the ARM64 header's
`image_size` is `FD_SIZE`. Appending an `XNUP` payload after the FD makes the kernel
longer than `image_size`; sboot rejects it / does not load the tail → black screen →
Download Mode.

**Fix:** never append after the FD. Put the payload **inside** the FD's unused tail
(FD offset `0x100000` → physical `0xE8100000`; the FV's files end at ~`0x79448` and
the FD is 2 MB).

### 5.3 Writing `CNTFRQ_EL0` from EL1 traps

The firmware leaves `CNTFRQ_EL0 = 0`, which makes every timeout a division by zero.
Writing it to fix that is an **illegal instruction at EL1** (`synchronous exception`
at the `msr`). It is only writable from EL2/EL3.

**Fix:** never write it. `timer_freq()` returns `CNTFRQ` or 26 MHz (Exynos 990's arch
timer) for the loader's own timing and for the device tree's `timebase-frequency`.

### 5.4 `GIC_V2=1` not exported → a GICv3 kernel

`./xnu-build.sh` is a **child process**. Sourcing `common/devices/y2s.conf` sets a
shell variable that the child never sees, so `MACHINE_FLAGS_ARM64_BCM2837` never got
`-DGENERIC_GIC_V2_IRQ=1` and the kernel was built with GICv3. It dies immediately
after `pe_arm_init_debug` (silently — GICv3 on a GIC‑400).

**Fix:** `export GIC_V2=1` before building, and force the dependent objects to
rebuild (they do not recompile when only a `-D` changes):

```sh
grep -rl 'board_config.h' osfmk pexpert bsd | xargs touch
touch osfmk/arm64/gic_v2_irq.c pexpert/arm/pe_fiq.c
rm -f BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
```

Verify with `strings mach.development.bcm2837 | grep 'gic: v'` — it must say
`gic: v2`. (Note: `gic_v3_irq_cpu_init` is the *generic* entry‑point name; when
`GENERIC_GIC_V2_IRQ` is set it is defined by `gic_v2_irq.c`. So the symbol name is
not a reliable indicator — the string is.)

### 5.5 `loader_return_after` does not fire on this phone

`loader_return_after=N` works under QEMU but does **not** return control on the
phone (tested with 25 s; iOS kept running). So it cannot be used as a log trigger.
See §9 for what we use instead.

---

## 6. The UEFI (`Mu-Silicium`, `y2sPkg`)

`Mu-Silicium/Platforms/Samsung/y2sPkg` is a copy of `x1sPkg` with:

- new platform GUID, S20+ SMBIOS strings, `PLATFORM_NAME = y2s`
- `y2s.dec`, `y2s.dsc`, `y2s.fdf`, `DeviceBuild.py` (all mechanical copies)
- **ACPI kept**, pointing at `x1s/AcpiTables.inf` from `Silicium-ACPI`
- `Resources/Configs/y2s.toml`, `Resources/DTBs/y2s.dtb` (packing config; the DT is
  not used by the FD)
- `Library/MemoryMapLib/MemoryMapLib.c` identical to x1s plus one descriptor:

```c
{"XNU Download", 0xF0000000, 0x01000000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},
```

That 16 MiB window (ending exactly at the display at `0xF1000000`) is where the
loader stages the 15.2 MB kernel read from storage.

### The loader hook

`Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/BootManager.c`:

```c
  DeviceBootManagerUnableToBoot ();
  XnuLoaderMain ();              /* does not return if the loader starts */
```

`XnuLoader.c` (same directory) walks the XNUP payload at `0xE8100000`, finds
`loader.efi`, `LoadImage`/`StartImage`s it, prints every step to the console, and
cold‑resets if nothing started. `BootManagerLib.inf` needs
`XnuLoader.c` in `[Sources]` and `gEfiBlockIoProtocolGuid` +
`gEfiPartitionInfoProtocolGuid` in `[Protocols]` (a reverted `.inf` loses them and
the link fails with `undefined symbol: gEfiPartitionInfoProtocolGuid`).

### Building it

`Mu-Basetools` has no macOS host support, so the UEFI is built in an **arm64 Ubuntu
Lima VM** (`hackios`):

```sh
limactl shell hackios -- bash -lc \
  'cd /path/to/y2s-workspace/Mu-Silicium && venv-linux/bin/python build_uefi.py -d y2s'
```

Output: `Mu-Silicium/Build/y2sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` (2 MiB,
59 images verified). A build takes ~10 s incremental.

---

## 7. The loader (`xnuloader`)

Source: `hackios-xnu-kernel-main/xnuloader/` (`loader.c`, `efi.h`,
`mkpayload.py`, `build.sh`). Built as a freestanding AArch64 UEFI app,
`/base:0xA1000000`, ~69 KB.

What it does on the phone (from its own console log):

```
== XNULOADER v3 ==
CurrentEL=1 MIDR=412fd050 MMFR0=101122 CNTFRQ=26000000 image=a1000000
display 1440 x 3200 at f1000000
platform y2s
payload entry 'loader.efi' size 10e00
payload entry 'cmdline' size b
payload entry 'kpart' size 9   ... 'koff' ... 'rpart' ... 'roff' ... 'rsize' ...
payload entry 'rdpart' size 6  ... 'rdoff' ... 'rdsize' ...
kernel: read 1000000 bytes from 'recovery' at 2000000
kernel va fffffff007004000..fffffff008328000 entry fffffff0074274a8
largest free block 880000000 size 27e000000
phys window 880000000 size 180000000, claim status 0
resident image: reading 948c0000 bytes from 'super'+184b00000
resident image: read
kernel loaded at pa 883004000..884328000
ramdisk at pa 884328000 size 400000
resident image at pa 888000000 size 948c0000: it is the root (md0), the payload ramdisk is md1
device tree 91c8c0000 len 38ac, boot_args 91c8d0000, top 91c8d4000
cmdline: debug=0x8 cpus=1 rd=md0 touch_ts=1
jumping to 8834274a8
```

### Payload entries (`mkpayload.py`)

| Entry | Meaning |
|---|---|
| `loader.efi=…` | the loader itself, started by the UEFI hook |
| `kpart` / `koff` | GPT partition + byte offset of the XNU kernel |
| `rpart` / `roff` / `rsize` | partition + offset + size of `root.apfs` (md1) |
| `rdpart` / `rdoff` / `rdsize` | partition + offset + size of `system.apfs` (md0, resident) |
| `cmdline` | extra boot args |
| `usb` | stay resident and take fastboot commands (**does not work on this phone**, see §10) |
| `rdres` | size of a resident image already in RAM (QEMU) |

### The XNUP payload format

```
u32 magic "XNUP" (0x50554E58), u32 size
repeated: char name[16], u32 size, u32 reserved, data padded to 16
terminator: a zeroed 24-byte entry
```

---

## 8. The kernel

### Build (native macOS)

```sh
cd hackios-xnu-kernel-main
. common/macos-env.sh          # GNU coreutils/tar/sed + project shims
export GIC_V2=1                # MUST be exported (see §5.4)
./xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k
```

- Tree: `xnu-src/xnu-8796.121.2` (Apple XNU + `redwood.patch` + `uefi/files`)
- Config name is still `BCM2837`; `MACHINE_FLAGS_ARM64_BCM2837` gets
  `-DGENERIC_GIC_V2_IRQ=1` when `GIC_V2=1`
- Output: `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`
  (~15,987,840 bytes)
- The build **always** ends with `check_all_exports` failures on
  `config/*.symbolset` ("exported name not in import list: _pmap_iommu_*", …).
  **This is expected and harmless** — the kernel binary is produced and is valid.
  Do not chase it.

macOS specifics (all documented in `docs/MACOS.md`): `xnu-deps/host-macos/bin/xcrun`
shim for `migcom`, `XNU_NO_TYPE_BUILTINS 0`, BSD `install` shim, `MEMORY_SIZE`
fallback, `python` shim, host‑aware `xnu-clang`.

### What makes it boot on this phone

- **GICv2 driver**: `osfmk/arm64/gic_v2_irq.c` (written for this port). QEMU's GICv2
  model is unfaithful (no security extensions, `GICD_TYPER=0x68`, acks `1022`), so
  QEMU can only prove it compiles, not that it works. On the phone it works:
  `gic: v2, distributor 0xfffffff19d815000, cpu interface 0xfffffff19d81a000 (IIDR 0x0202143b)`.
- **Device tree** written by the loader (CPU nodes with the explicit MPIDR table,
  GICv2 node, memory, chosen/boot‑args, timebase 26 MHz).
- **Boot args**: `debug=0x8 cpus=1 rd=md0 …`.

### Redwood drivers already in the tree

`bsd/dev/redwood_*.c` — a whole driver framework inherited from the (Qualcomm)
upstream port: `redwood_input.c` (touch injection + `/dev/redwood-touch`),
`redwood_goodix.c` (Goodix/FocalTech touch, GENI SPI + RPMh — Qualcomm only),
`redwood_buttons.c`, `redwood_battery.c`, `redwood_ufs.c`, `redwood_adreno.c`
(GPU), `redwood_audio.c`, `redwood_camera.c`, `redwood_torch.c`,
`redwood_virtio_*` (QEMU), and now **`redwood_sec_ts.c`** (ours).

---

## 9. Debugging methodology (this is the reusable part)

The kernel log is the only channel, and it is awkward:

- The loader writes the kernel log to `recovery`+16 MiB **only when the kernel
  returns** (panic or `loader_return()`).
- `loader_return_after` does not fire on this phone (§5.5).
- The loader also clears the screen and prints the log, but the console shows the
  **tail**, so early lines scroll away.

So the practical loop is:

1. **Panic on purpose.** The driver calls `panic("sec_ts: … %#x …", …)` with the
   numbers in the message. The panic is the last thing in the log, so it is on
   screen and in storage.
2. Read it back reliably: force off → Download Mode → flash stock `misc` →
   Android → `dd if=/dev/block/by-name/recovery bs=1M skip=16 count=2` → pull.
3. Note that the loader's own diagnostics (partition reads, DT, window) are always
   in that same log, above the kernel part.

Two other useful tricks found this session:

- **Write‑back test**: write a value into a hardware register and read it back. If it
  does not stick, the block is not clocked/powered — this is what finally explained
  the HSI2C failure.
- **Ghost‑address test**: send an I2C address nobody owns. If it "ACKs", the data
  line is stuck low and every previous ACK was fake.

---

## 10. Touch — full technical report

### 10.0 Resolution (supersedes 10.1–10.6)

Touch works (`touch_ts=1`). Three assumptions below turned out to be wrong:

- The controller is **Samsung LSI S6SY79A** (`sec_ts`, `tsp_sec/y79a_y2.bin`), **not**
  STM FingerTipS. Protocol = Samsung `drivers/input/touchscreen/sec_ts/y79a`
  (16‑byte events, `0x60/0x61` read events, `0x10` sense on, …).
- The S2DOS05 PMIC is on **hsi2c_4 = gpp1‑0/1 (PERIC0 `0x10430000`+`0x20`)**, a
  different bus from touch. Bit‑banging it needs no CMU clock, so there was no
  circular dependency. Enable LDO2 (`tsp_io`) then LDO3 (`tsp_avdd`): `EN` reg `0x03`
  bits 1/2.
- HSI2C_15's USI switch is `0x10721024` (SYSREG_PERIC1), not `0x10421050`.

Full details are in `hackios-xnu-kernel-main/docs/TOUCH.md`. The driver is
`bsd/dev/redwood_sec_ts.c`. The CMU_PERIC1 question in 10.5/10.6 is moot for touch.


### 10.1 The hardware

| | |
|---|---|
| Controller | **STMicroelectronics FingerTipS** (Samsung `sec_ts`, firmware `y79a_y2`) |
| I2C address | `0x48` |
| Bus | Samsung **HSI2C_15** @ `0x108D0000` (PERIC1) |
| Pins | **gpp5‑6 = SCL, gpp5‑7 = SDA**, function 3 (I2C) |
| Pinctrl | PERIC1 @ `0x10730000`, GPP5 bank at offset `0x0` |
| USI routing | `0x10421050` = `0x4` (USI v2 → I2C) — **already set** |
| Attention | `gpa1‑0`, falling edge (`sec,irq_type = 0x2008`) — unused (we poll) |
| Coordinates | `sec,max_coords = <0x1000 0x1000>` → 0…4095 on both axes |
| Supplies | `tsp_io` (S2DOS05 LDO2, 1.8 V), `tsp_avdd` (S2DOS05 LDO3, 3.0 V) |

The supplies live on an **S2DOS05** PMIC (`s2dos05_pmic@60`, I2C `0x60`), both marked
`regulator-boot-on`.

### 10.2 The protocol (from mainline `drivers/input/touchscreen/stmfts.c`)

Command‑then‑read: write one command byte, repeated start, read N bytes.

| Command | Value |
|---|---|
| read info block | `0x80` |
| read all events | `0x86` (up to 32 events) |
| read one event | `0x85` |
| system reset | `0xa0` |
| sleep out | `0x91` |
| MT sense on | `0x93` |
| clear event stack | `0xa1` |

Info block (8 bytes): `[0]` chip version, `[2..3]` firmware (BE), `[4]` config id,
`[5]` config ver, `[6..7]` chip id (BE).

Event (8 bytes):

```
slot = (e[0] & 0xF0) >> 4
x    = e[1] | ((e[2] & 0x0F) << 8)
y    = (e[2] >> 4) | (e[3] << 4)
maj/min/orientation/area = e[4..7]
```

Event ids: `0x02` MT detected, `0x03` enter, `0x04` leave, `0x05` motion,
`0x0E` key, `0x0F` error, `0x10` ready, `0x16` status.

### 10.3 The driver (`bsd/dev/redwood_sec_ts.c`, ~18 KB)

- Wired into `redwood_input_init()` (`bsd/dev/redwood_input.c`) and
  `bsd/conf/files`.
- Boot args: `touch_ts=1` probe + poll, `touch_ts=2` probe only, `touch_ts=4` full
  HSI2C reset + fresh timing; plus `touch_poll_us`, `touch_flip`, `touch_trace`.
- Transport: **I2C bit‑banged on gpp5‑6/7** (the HSI2C controller cannot be used,
  see 10.5). GPIO registers: `CON +0x00` (4 bits/pin), `DAT +0x04`, `PUD +0x08`;
  function 0 = input, 1 = output; pull‑up = 3; ~125 kHz.
- Decoding posts to `redwood_touch_post(type, slot, x, y)` with x/y scaled to
  0…65535 (×65535/4095), `touch_flip` mirrors.
- `redwood_input.c` queues the samples for the window server and implements a
  built‑in escape: **five fingers held 1.5 s returns control to the loader**.

### 10.4 What is proven to work

- Pins mux to output and drive both lines high (`CON 0x11000000`, `DAT 0xe3`).
- Pull‑ups take effect (`PUD 0x1101f111`).
- The bit‑bang transport runs, clocks bytes, and reads ACK bits.

### 10.5 The blocker (measured, not guessed)

```
sec_ts: after setup: CON 0x11000000 DAT 0xe3 PUD 0x1101f111 (both lines high)
sec_ts: SDA as input reads 0 (1 = high)
sec_ts: ghost address 0x55 acked=1 (must be 0)
sec_ts: SLEEP_OUT -> 1 / SYSTEM_RESET -> 1 / SLEEP_OUT -> 1
sec_ts: info 00 00 00 00 00 00 00 00
```

- **SDA is held low**: as an input it reads 0 despite a pull‑up, and a non‑existent
  address "ACKs". Every ACK was fiction; every byte read as 0.
- At boot the pins already read low as inputs (`CON 0 DAT 0x23`).
- Conclusion: **the touch controller is unpowered** (its pins clamp the bus). The
  `regulator-boot-on` rails are not actually on in our boot path, and XNU has no
  PMIC driver for this phone.

Before that, the HSI2C controller route was also ruled out, with evidence:

```
sec_ts: as found: CTL 0x8 CONF 0x980110ff FS3 0xff0000
sec_ts: USI_15 was 0x4, now 0x4 (I2C)
sec_ts: CMU PERIC1 bus 0x200000 -> 0x300000, ip 0x200000 -> 0x300000
sec_ts: scratch: trail 0xffffff->0xffffff addr 0xff0fffff->0xff0fffff timeout 0x800000ff->0x800000ff
```

All three registers hold their **reset defaults** and ignore writes: the block is
held in reset with its **IP clock gated**. CMU_TOP's PERIC1 domain gates
(`0x20d4`/`0x20d8`, bit 21, with bit 20 = manual, i.e. `CMU_GATE_ON = 0x300000`)
write fine but are not enough. Exynos 990 has a **separate `cmu_peric1` clock
controller** (mainline: `samsung,exynos990-cmu-peric1`) that holds the per‑IP
`hsi2c` gate — and the phone's downstream device tree only exposes CMU_TOP
(`clock-controller@0x1a330000`), so **its base address is the one missing fact**.

### 10.6 Why it is circular, and the critical path

To power the touch we must program the S2DOS05 **over I2C** → which needs a working
I2C controller → which is the block whose clock we cannot find. So:

1. Find `cmu_peric1`'s base + the `hsi2c` gate bit (Samsung's downstream clock
   driver, or the mainline `exynos990-cmu-peric1` series). Verify with the scratch
   test: `trail 0xffffff` must become `0x0badf00d`.
2. Enable `tsp_io` / `tsp_avdd` through the S2DOS05 (`0x60`).
3. The FTS probe then answers; the rest of the driver (decode + inject) is written.

### 10.7 HSI2C register map (for when the clock is solved)

From Samsung's own `EFI_HSI2C_BUS` struct in
`Mu-Silicium/Silicon/Samsung/SamsungPkg/Drivers/HsI2cDxe/HsI2c.h` (identical to
mainline `i2c-exynos5.c` for this generation):

| Offset | Register | Notes |
|---|---|---|
| `0x00` | ctrl | bit0 FUNC_MODE_I2C, bit3 MASTER, bit6 RXCHON, bit7 TXCHON, bit31 SW_RST |
| `0x04` | fifo_ctrl | RXEN bit0, TXEN bit1, RX trig `<<4`, TX trig `<<16` |
| `0x08` | trail_ctrl | reset value `0xffffff` |
| `0x20` | int_enable | |
| `0x24` | int_status | DONE bit7, ABORT bit8, NO_DEV_ACK bit9, NO_DEV bit10, TIMEOUT bit11 |
| `0x2c` | err_status | |
| `0x30` | fifo_status | TX lvl bits 6:0, RX lvl bits 22:16, empty bits 8/24 |
| `0x34` | tx_data | |
| `0x38` | rx_data | |
| `0x40` | conf | AUTO_MODE bit31, 10BIT bit30, HS bit29 |
| `0x44` | auto_conf | READ_WRITE bit16, STOP bit17, MASTER_RUN bit31, length bits 7:0 |
| `0x48` | timeout | EN bit31, reset value `0x800000ff` |
| `0x50` | trans_status | DONE bit0, ABORT bit1, NO_DEV_ACK bit2, NO_DEV bit3, TIMEOUT bit4 |
| `0x54..0x6c` | timing_hs1..sla | |
| `0x70` | address | SLV_ADDR_MAS `(addr&0x3ff)<<10`, reset value `0xff0fffff` |
| `0xC4` | usi_con | Samsung writes 0 here |

Timing for 400 kHz from a 200 MHz source (mainline formula, Exynos5 path):
`temp = 200e6/400e3 - 8 - 2*ftl = 492`, `div = 0`, `cycle = 490`, `lo = min(cycle*64/100, 0xff) = 255`,
`hi = 235`, `su = 255`, `dh = 127` →
`FS1 = 0xffffff00`, `FS2 = 0x7f00ffeb`, `FS3 = 0x1ea`, `SLA = 0x7f`.

---

## 11. GPU — the honest assessment

`bsd/dev/redwood_adreno.c` (4125 lines) is entirely Qualcomm‑specific: Adreno 642L,
GMU, RPMh, GENI, SMMU. The S20+ has a **Mali‑G77 MP11**.

**Correction: hardware acceleration is not a dead end.** Upstream does not run
Apple's Metal driver. Its stack is:
Mesa (gallium + OSMesa, `hackios-xnu-userland/tools/mk-mesa.sh`), then
`tools/mesa/redwood-drm` (libdrm ioctls → IOKit user client `RedwoodGPU`), then a
kernel driver implementing the Linux DRM UAPI. `eagl.c` serves GLES to CoreAnimation,
and `tools/mesa/air` compiles Metal AIR shaders through NIR. The GPU-specific
parts are the Mesa gallium driver and the kernel UAPI.

For Mali that means **Mesa Panfrost** plus a panfrost-DRM-UAPI driver in XNU,
ported from Linux `drivers/gpu/drm/panfrost` (job manager, Mali MMU, BOs). Mesa
lists Valhall v9 (G57 `9.0.1`, using G77 counters) but not the G77 itself (`9.0.0`,
Arm "TTRx"). That needs a model entry and errata checks.

SoC bring-up facts (from `dtb.img`):

| | |
|---|---|
| Mali registers | `0x18500000`, size `0x5000`; `sysreg_g3d` `0x18420000` |
| IRQs (SPI) | JOB `0x12d`, MMU `0x12e`, GPU `0x12c` |
| Power domain | `pd-embedded_g3d` PMU `0x15862200` (status reg offset `0x2204`, mask `0x1`); `need_smc = 0x18410204`, so an EL3 SMC is needed after power-on to open the domain to non-secure |
| Clock | CMU CAL id `0x0b04000a` (DVFS through ACPM) |

Do not read `0x18500000` while the domain is off (SError). Bring-up order: PMU
domain on → SMC → clock → `GPU_ID` → Panfrost-style init.

Until that exists the system uses **software rendering** (OSMesa softpipe), which is
why the home screen appears.

This also explains the cosmetic gaps the user noticed:

- **No wallpaper**: iOS 16 composites the wallpaper as a GPU layer in a separate
  process; without GPU surfaces it comes out black (icons/text are CPU layers and
  render fine).
- **Blank Weather/Calendar widgets**: `weatherd` needs network + location (no
  Wi‑Fi/cellular driver yet), the Calendar has no events, and widget rendering also
  goes through a GPU‑backed render server.

---

## 12. Recovery (memorise this)

### Key combos

- **Force reset / power off**: hold **Volume Down + Power** ~10–15 s. (Power alone
  does not force off from a hung UEFI screen.)
- **Download Mode**: with the phone off, hold **Volume Down + Power** ~6 s → warning
  screen → press **Volume Up**.

### Back to Android

```sh
cd /path/to/y2s-workspace
BIN=heimdall-arm64/heimdall-frontend.app/Contents/MacOS/heimdall
"$BIN" flash --MISC samloader-rs/img/misc.bin --stdout-errors
```

That clears the boot‑recovery flag in `misc`; a normal reboot then boots Android.

**Never zero the whole 1 MiB `misc` partition.** The stock `misc.bin` is only
~520 KB and holds Samsung data; zeroing it produces *"An error has occurred while
updating the device software"* on every boot. Restore the stock file.

`adb reboot recovery` **sets the boot‑recovery flag**, and the UEFI does not clear
it, so afterwards every reboot goes back to the UEFI. Flashing stock `misc` is the
fix.

`adb root` is lost on every reboot — always re‑root before `dd`.

### Emergency: Android won't boot

`heimdall flash --RECOVERY y2s-recovery-backup.img --BOOT y2s-boot-backup.img` (both
backups are in the workspace root), or reflash the stock images from
`samloader-rs/img/`.

---

## 13. Runbook: build → ship → observe

```sh
cd /path/to/y2s-workspace/hackios-xnu-kernel-main

# 1. build the kernel (GIC_V2 must be EXPORTED)
. common/macos-env.sh
export GIC_V2=1
./xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k      # ignore the symbolset errors

# 2. stage the kernel
cp xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 out/mach.y2s

# 3. rebuild the recovery image: UEFI@0, kernel@32MiB, root.apfs@48MiB
python3 - <<'PY'
ker=open('out/mach.y2s','rb').read()
rd=open('userland/root.apfs','rb').read()
img=open('out/y2s-embed.img','rb').read()
out=bytearray(max(0x2000000+len(ker), 0x3000000+len(rd)))
out[0:len(img)]=img
out[0x2000000:0x2000000+len(ker)]=ker
out[0x3000000:0x3000000+len(rd)]=rd
open('out/y2s-recovery.img','wb').write(out)
PY

# 4. write it from Android (no Download Mode needed)
adb root && adb push out/y2s-recovery.img /data/local/tmp/r.img
adb shell 'dd if=/data/local/tmp/r.img of=/dev/block/by-name/recovery bs=1M; sync'
adb reboot recovery

# 5. read the log (after the panic, from Android)
adb shell 'dd if=/dev/block/by-name/recovery of=/data/local/tmp/l.bin bs=1M skip=16 count=2'
adb pull /data/local/tmp/l.bin ./xnulog.bin
python3 -c "d=open('xnulog.bin','rb').read().split(b'\0',1)[0]; print(d.decode('utf-8','replace'))"
```

If the kernel panics, the phone lands in Download Mode → flash stock `misc` →
Android → step 5.

---

## 14. File inventory

### Root of the workspace

| Path | What |
|---|---|
| `handoff.md` | this document |
| `PLAN.md` | the original analysis and phased plan |
| `Mu-Silicium/` | the UEFI tree (contains `Platforms/Samsung/y2sPkg`) |
| `hackios-xnu-kernel-main/` | the kernel + loader + userland project (main working tree) |
| `hackios-xnu-userland-main/` | userland sources (separate tree) |
| `android_device_samsung_exynos990-android-12.1/` | Android device trees incl. **prebuilt `dtb.img`/`dtbo.img`/`Image` for y2s** — the source of every hardware fact |
| `device_samsung_y2s-fourteen-qpr3/` | PixelOS device tree |
| `heimdall-arm64/` | working Heimdall (fathonix arm64 build) |
| `samloader-rs/` | firmware download + Odin‑style flash; `fw/` = 15 GB stock firmware, `img/` = extracted small images |
| `silicium-prebuilt/Mu-x1s.img` | the **known‑good prebuilt UEFI boot image** (112‑byte shim + 2 MB FD) |
| `y2s-boot-backup.img`, `y2s-recovery-backup.img` | the phone's original boot/recovery |
| `xnulog.bin` | last kernel log pulled from the phone |
| `build-uefi-y2s-in-vm.sh` | (legacy) builds the UEFI in the Lima VM; recreates the venv — prefer the `limactl shell` command in §6 |

### `hackios-xnu-kernel-main/`

| Path | What |
|---|---|
| `docs/Y2S-PROGRESS.md` | port log: boot chain, recovery, what works |
| `docs/TOUCH.md` | **the touch report** (hardware, protocol, evidence, blocker) |
| `docs/MACOS.md` | native macOS build guide |
| `docs/GICV2.md` | GICv2 driver design + QEMU findings |
| `out/mach.y2s` | current XNU kernel (15,987,840 bytes) |
| `out/y2s-embed.img` | UEFI boot image (shim + FD + XNUP payload) |
| `out/y2s-recovery.img` | **the image to flash**: UEFI@0 + kernel@32 MiB + root.apfs@48 MiB |
| `out/y2s-payload.bin` | the XNUP payload |
| `xnu-src/xnu-8796.121.2/` | the patched XNU tree (build output under `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/`) |
| `xnu-src/.../bsd/dev/redwood_sec_ts.c` | **the touch driver we wrote** |
| `xnu-src/.../bsd/dev/redwood_input.c` | touch injection core + `/dev/redwood-touch` |
| `xnu-src/.../osfmk/arm64/gic_v2_irq.c` | GICv2 driver |
| `xnu-src/.../bsd/conf/files` | where `redwood_sec_ts.c` is registered |
| `xnuloader/loader.c` | the loader |
| `xnuloader/build.sh` | builds `loader.efi` (`GIC_V2=1 ./build.sh`) |
| `xnuloader/mkpayload.py` | builds the XNUP payload |
| `userland/root.apfs` | 4 MiB `/redwood` disk (md1) |
| `userland/system.apfs` | 2.32 GB iOS root (md0) |
| `qemu/run-ios.sh` | QEMU baseline |
| `common/devices/y2s.conf` | device profile (FD base/size, payload base, `GIC_V2=1`, `FB_BASE`) |
| `pack-y2s.sh`, `flash-y2s.sh` | older pack/flash helpers (superseded by §13) |

### `Mu-Silicium/` (the parts that matter)

| Path | What |
|---|---|
| `Platforms/Samsung/y2sPkg/` | our platform package (`y2s.dsc`, `y2s.fdf`, `MemoryMapLib`, `KeypadDeviceLib`, `DeviceBuild.py`) |
| `Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/BootManager.c` | calls `XnuLoaderMain()` |
| `.../BootManagerLib/XnuLoader.c` / `.h` | the UEFI-side loader hook (`XNU_PAYLOAD_BASE 0xE8100000`) |
| `.../BootManagerLib/BootManagerLib.inf` | must list `XnuLoader.c` + the two protocol GUIDs |
| `Silicon/Samsung/SamsungPkg/Drivers/HsI2cDxe/HsI2c.h` | **the authoritative HSI2C register map** |
| `Silicon/Samsung/S5E9830Pkg/Library/HsI2cLib/HsI2c.c` | per‑bus pin map (hsi2c_15 = gpp5‑6/7) |
| `Silicon/Samsung/S5E9830Pkg/Library/GpioBankLib/GpioBank.c` | GPIO bank offsets (GPP5 = 0x0 in PERIC1) |
| `Silicon/Samsung/S5E9830Pkg/Library/PlatformMshcLib/Cmu.h` | CMU_TOP base + `CMU_GATE_ON` semantics |
| `Build/y2sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd` | the built UEFI |

---

## 15. Next steps, in order

1. ~~Touch~~ — **done** (see §10.0).
2. Then, roughly in order of value: storage (`UfsDxe` exists), buttons
   (`redwood_buttons.c` pattern), audio, and a USB config driver so the loader can
   serve fastboot (`UsbfnDwc3Dxe` gives `gEfiUsbFunctionIoProtocolGuid`; the loader
   currently looks for the Qualcomm GUID and has no USB config driver to attach to).

---

## 16. Gotchas worth remembering

- `export GIC_V2=1` — otherwise you build a GICv3 kernel that dies silently.
- The symbolset `check_all_exports` failure is **normal**; the kernel is fine.
- `stat` is GNU here: `stat -c %s`, not `-f %z`. Prefer `wc -c`.
- `adb root` is lost on reboot.
- Never zero all of `misc`; restore the stock file.
- `adb reboot recovery` sets a sticky boot flag; stock `misc` clears it.
- The UEFI's console shows only the **tail** of a long log.
- `loader_return_after` does not work on this phone.
- Appending to the FD breaks sboot; put payloads **inside** the FD.
- Removing ACPI breaks BDS entirely (dependency expression).
- `ml_io_map` + a write-back test is the fastest way to tell "wrong offset" from
  "block not clocked".
- A ghost I2C address is the fastest way to tell "no device" from "SDA stuck low".
