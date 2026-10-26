# 06 — Wireless: Wi‑Fi, Bluetooth, cellular

Parallel to Metal, at lower priority. Research notes and source copies are in the session's
`files/wireless-ref/`: z3s `WIFI_BRINGUP.md`, `BLUETOOTH.md`, the brcmfmac patches, and Samsung's
PCIe CAL.

## Hardware (y2s device trees + z3s, the same SoC)

| | |
|---|---|
| Combo chip | **Broadcom BCM4375 B1** (Wi‑Fi 6 + BT), PCIe id `14e4:4475` |
| Wi‑Fi bus | PCIe ch 0, `pcie@133B0000`, Samsung **host‑v0** controller (`samsung,exynos-pcie`): ELBI `0x133b0000`, PHY `0x133e0000`, PCS `0x133d0000`, DBI `0x133f0000`, sysreg `0x13021044`; 1 lane; **legacy INTx** (`use-msi=false`) |
| Wi‑Fi GPIOs | REG_ON **gpb0‑4**, host‑wake **gpa3‑7** |
| Firmware (vendor partition) | `/vendor/firmware/bcmdhd_sta.bin_b1`, `bcmdhd_clm.blob`, `nvram.txt_*_b1` |
| BT bus | **UART1 at 0x10840000** (USI v2, UART mode via SYSREG_PERIC1 0x10721000; SPI 416; not serial_16), BT_EN **gpm16‑0**, dev‑wake **gpg1‑1**, host‑wake **gpa2‑3**; firmware `.hcd` (the core reports as `BCM4362A2`) |
| Modem | Shannon 5123 CP, shared memory `0xf0000000` (8 MB) + `0xd0000000` (64 MB), mailbox IPC (`exynos,modem_if`) |

## Wi‑Fi: port z3s's working Linux recipe through upstream's LinuxKPI shim

Upstream ran Linux's ath11k inside XNU (`redwood_lx.c`). About 70 % of that stack is chip‑agnostic:

- `redwood_net.c`: the en0 interface
- `redwood_sta.c`: scan/join state machine behind an ops vtable
- `redwood_apple80211.c`: the whole wifid/Apple80211 emulation
- the LinuxKPI core helpers

What changes is the bus and the driver:

| ID | Step |
|---|---|
| W1 | **Done (2026‑10‑25), without the phone:** `fw-stock/vendor/firmware/` has `bcmdhd_sta.bin_b1` (18.41.117), `bcmdhd_clm.blob`, 12 `nvram.txt_*` (Murata `1rh_es4x_b1` and SEMCO `CS01_semco_b1` are the B1 ones; which module this phone has is read from the chip/OTP in W2, or by trying both), the BT `bcm4375B1_{murata,semco}.hcd`. Next: ship the B1 files in the image's `/redwood/fw/` |
| W2 | `redwood_pcie_exynos.c`: host‑v0 RC bring‑up (PHY cal from Samsung `pcie-exynos9830-v0-cal.c` / z3s `kernel/drivers/pcie-exynos/`), WLAN_REG_ON, PERST, link up, config space, BAR. Measured by reading `14e4:4475` from config space |
| W3 | **S2MPU** DMA grants: SMC `0xc6000101`, arg `(1<<16)\|24`, per 64 KB granule of every DMA buffer (z3s patch 0001) |
| W4 | brcmfmac (PCIe backend) built against the LinuxKPI shim, as `tools/mk-ath11k.sh` does for ath11k. z3s fixes: legacy INTx, `host_cap = 0x00821407` |
| W5 | `redwood_sta` ops for brcmfmac (scan/join via cfg80211 calls), then wifid: networks listed, WPA2 join, DHCP |

The biggest blocker is W2+W4 (PCIe controller and brcmfmac through LinuxKPI). It is large but de‑risked: every non‑obvious step is solved and documented by z3s on this SoC.

### Order of work and what each run proves

1. **W2a probe (read‑only)**, boot‑arg `wifi_pcie=1`: map ELBI/DBI/PHY/PCS and the PMU PCIe isolation, log the link state S‑Boot left (likely all off), read the WLAN_REG_ON GPIO (gpb0‑4) state.
2. **W2b link up**, `wifi_pcie=2`: REG_ON high → 100 ms → PHY cal (Samsung `pcie-exynos9830-rc-cal.c` table) → PERST# release → LTSSM enable → wait for L0. Done when config space reads `14e4:4475` and BAR0/BAR2 sizes.
3. **W3 + W4a firmware download**, `wifi_pcie=3`: S2MPU grants for one DMA buffer, brcmfmac's PCIe backend in LinuxKPI (reuse `wifi/lx` + `redwood_lx.c`), firmware + NVRAM upload through BAR2 (TCM), the chip's shared‑area handshake. Done when the firmware's version string (`wl ver`) is in the log.
4. **W4b/W5**: scan results in the log → `redwood_sta` ops → wifid lists networks → join WPA2 → DHCP (`net_test`).

Upstream pieces reused unchanged: `redwood_net.c` (en0), `redwood_sta.c` (scan/join state machine), `redwood_apple80211.c` (wifid's Apple80211 ioctls), `wifi/lx` (LinuxKPI headers/glue), `userland/src/net_test.m`. Upstream pieces **not** reused: `redwood_wifi.c`/`redwood_wlfw.c`/`redwood_qmi.c` (Qualcomm WCN6750 + QMI).

### W2/W3 implementation (2026‑10‑25)

`bsd/dev/redwood_exynos_pcie.c` (written by an agent, reviewed). Nothing runs without the boot-arg:

| Boot-arg | What it does |
|---|---|
| `wifi_pcie=1` | Read-only look: PMU PHY isolation (`0x15860718`), CMU_TOP/CMU_HSI1 PCIe mux and gates, sysreg, pins (REG_ON gpb0‑4, PERST# gpf0‑1, CLKREQ gpf0‑0, host-wake), ELBI/PHY/PCS/DBI. A block is skipped if its clock is gated by hand or its PHY is isolated, unless `wifi_pcie_force=1`. |
| `wifi_pcie=2` | The look, then after `wifi_pcie_wait` seconds (default 20: the system is up, so a stall still lets Vol‑Down+Power save the log): PERST# low, REG_ON low 20 ms, high, 200 ms; HSI1 PCIe clocks forced on; PHY isolation off; PHY tables (ch0, 26 MHz, Gen2); controller and root port set-up; PERST# released; LTSSM on; L0 (two tries); iATU CFG0; the endpoint id and BAR sizes. |
| `wifi_pcie=3` | As 2, then **W3**: the loader's DMA pool granted to HSI1 through Samsung's hypervisor, `hvc(0xc6000101, (1<<16)|24, granule, 0x10000, 3)` per 64 KB. |

**DMA pool.** z3s's S2MPU grants (their `s2mpu.c`) cover only low RAM, `0x80000000–0x100000000`. Our kernel's
memory is entirely above 4 GB. So on y2s the loader takes 32 MB below 4 GB from UEFI (`reserve_wifi_dma()`,
EfiLoaderData, 64 KB aligned) and publishes it in `/chosen` as `wifi-dma-base`/`wifi-dma-size`.
`redwood_wifi_dma_pool()` maps it normal non-cacheable (memcpy-safe). All brcmfmac DMA (coherent rings and
bounce buffers for streaming maps) comes from this pool.

What success looks like in the log (`wifi:` lines):
- `dma pool: <phys> size 2000000`
- `ltssm (try 1): UP` and `link up: ... Gen1 x1`
- `endpoint 01:00.0: id 14e4:4475`, plus BAR0/BAR2 sizes
- `W2b PASS`
- `W3 PASS: ... 512 granules granted, 0 refused`

z3s facts for W4 (sources in `files/wireless-ref/z3s_brcmfmac_*`):
- interrupts on legacy INTx (ELBI `0x000` status, `0x00c` enable), not MSI
- `host_cap = 0x00821407`, written at shared+84 before hostready
- RAM base `0x170000`
- firmware `18.41.117`

## Bluetooth

| ID | Step |
|---|---|
| B1 | UART 16 in the kernel (USI mux, clocks with `clkreq-on`, or bit‑level PL011‑like Samsung UART driver) |
| B2 | BT_EN + firmware patchram download (HCI `0xfc2e` / `0xfc4c`…, as Linux `btbcm`) |
| B3 | iOS side: how `bluetoothd` attaches an HCI transport (BlueTool / `/dev/uart.bluetooth`-style serial on real iPhones). Find out from the iOS 16.5 image first |

The biggest blocker was B3. It is now answered below: a kernel control socket + an IOService, with no tty and no Apple binary patched.

### B3 findings (2026‑10‑25, from the 20F66 image)

Disassembly/strings in the session's `files/bt-ref/{ios,work,tools}` (`xref.py <macho> str <s>` finds code refs).
Addresses in 1–3 and 6 are `/usr/sbin/bluetoothd`, in 4–5 `/usr/sbin/BlueTool` (both 20F66, `__TEXT` at
`0x100000000`). There is no `/etc/bluetool/` and no `.hcd` file in the image: the iPhone X's BT firmware
(`BCM4357B1_19.1.282.5153_Hercules_OS_MUR_20210524.hcd`, `…5154…USI…`) is **embedded in BlueTool**, with the
scripts. Launchd jobs: `com.apple.bluetoothd` (mobile, RunAtLoad, KeepAlive on failure) and `com.apple.BlueTool`
(`/usr/sbin/BlueTool -R`, mobile, MachService `com.apple.BlueTool`, started on demand).

1. **bluetoothd's "is there Bluetooth" gate** (main, `0x100026358`): needs a non‑zero BD address and
   `access("/dev/cu.bluetooth")` **or** `access("/dev/uart.bluetooth")` **or** an IOService named `marconi-bt`
   or `bluetooth`; otherwise it prints `No bluetooth on this device.` and exits 0 (launchd won't restart it).
   The address comes from `IOServiceNameMatching("bluetooth")` → `IORegistryEntrySearchCFProperty(…, "IODeviceTree",
   "local-mac-address", recursive)` (`0x100027238`), exported as env `BT_DEVICE_ADDRESS`.
2. **Transport choice** (`0x100155f8c`): env `BT_HCI_TRANSPORT="<transport>:<driver>:<devstring>"` overrides; else
   the device‑tree property `transport-encoding` (u32, read from IOService `marconi-bt`, then `bluetooth`,
   `0x1000e74b0`) selects: 0 USB, **1 H4**, 2 H4DS, 3 H4BC, 4 H5, 5 BCSP(→USB), 6 APPLEBT, 7 PCIe; built‑in
   default 3. Transports: USB H4DS H4BC H4 H5 APPLEBT VIRT PCIE PCIESW; drivers: USB POSIX APPLEBT PCIE PCIESW.
   Speed: env `BT_BAUDRATE`, else DT `transport-speed` (u32), else 2 400 000 (`0x1000e6fa4`).
3. **The UART ("POSIX") driver is not a tty** (`open` at `0x10006669c`): `socket(PF_SYSTEM, SOCK_STREAM,
   SYSPROTO_CONTROL)`, `ioctl(CTLIOCGINFO)` + `connect()` to a **kernel control** named by `<devstring>`
   (`"name,baud"`; default `com.apple.uart.sk.bluetooth`), then `getsockopt(fd, 2, TIOCGETA)` → `cfmakeraw`,
   `cfsetspeed`, CRTSCTS → `setsockopt(fd, 2, TIOCSETA)`. Bytes then go through plain `read()`/`write()` on that
   socket (RxLoop `0x1000673c8`, write `0x1000670d0`). Only when the name is exactly `com.apple.uart.sk.bluetooth`
   it also does `setsockopt IOAOSSKYSETCHANNELSPEC` / `getsockopt IOAOSSKYGETCHANNELUUID` and moves data over a
   Skywalk `os_channel` instead (AppleOnboardSerial "BM3UART").
   Termios: H4 (mode 0) sets `IGNPAR`, `CS8|CREAD|CLOCAL|CCTS_OFLOW|CRTS_IFLOW`; H5 sets even parity, no flow.
   Wake: `open("/dev/btwake")` held open = BT_WAKE asserted, `close` = deasserted (`0x1000665e4`); failure only logs.
4. **bluetoothd drives BlueTool over XPC** (`0x100271e60`): `{kMsgId:"RunBuiltinScript", kMsgArgs:{script, btaddress}}`
   to `com.apple.BlueTool`, with `script` = `boot` ("Booting chipset", `0x100272b6c`), `init` ("Powering on
   chipset") or `deepsleep` ("Powering off chipset"). BlueTool replies `{result:int64, manufacturer:int64,
   firmware:string}` (`0x10004260c`); bluetoothd treats `result 0` as success; its power‑on loop
   (`0x1003a5f54`) restarts on code `0xd5` ("Unknown platform/chipset/manufacturer combination") and `2`
   ("timed out booting the chip").
5. **BlueTool picks the script by platform and chip** (`runCommandForCurrentConfig`, `0x1000162a0`):
   - platform from `MGCopyAnswer(kMGQProductHash)` (`getPlatform`, `0x100040958`): iPhone10,3 = `Hercules`.
   - chip from its own `device -D` + HCI Read Local Version (`getChipset`, `0x100041094`): manufacturer 15
     (Broadcom), then a switch on LMP subversion (`0x420e` → `BCM4357B1`, …). An unknown subversion gives the
     bare string `BCM`, then there is no firmware entry (`fwPos == NO_FW_FOUND`), and the boot fails.
   - `/etc/bluetool/<hwmodel|platform>.<boot|init|deepsleep>.script` overrides are honoured only on
     `Internal`/`Vendor` installs (`0x100016474`).
   - Device: `device -D` opens **kernel control `com.apple.uart.bluetooth`** (env `BT_UART` overrides) at 115200
     (`0x10001d1e8`, open `0x10001dc34`): the same socket API as bluetoothd, non‑Skywalk name. Power/reset/wake are
     `/dev/btpoweroff`, `/dev/btreset`, `/dev/btwake`.
   - Built‑in Broadcom sequence (fragments at strings 86596–86613): `wake on`, `hci reset`, `bcm -B`
     (vendor baud), `device -D -S`, `bcm -W` (patchram), `device -s 115200`, `bcm -A` (BD_ADDR from DT),
     `bcm -N`, `bcm -s …` (sleep mode), `bcm -b 1500000`, `wake off`.
6. **Chip identity matters only to BlueTool.** bluetoothd itself just needs the BlueTool result and then speaks
   standard HCI over H4. If stock BlueTool ran on our chip (BCM4375B1 BT core = `BCM4362A2`, subversion not in
   its table), it would fail with `BCM`. If it ever matched `BCM4357B1`, it would load Apple's iPhone X
   patchram into our chip. Neither is acceptable, so stock BlueTool must not run.

Ruled out: there is no tty (`/dev/uart.bluetooth` is only an existence check), no H5, no `AppleConvergedIPC*`,
`AppleBluetoothModule`/`AppleBTHci` (PCIe/APPLEBT transports), and no Skywalk (avoided by naming the kernel control).

### B3 design (least effort)

**Kernel** (`redwood_exynos_bt.c` grows into the driver; plus a small IOKit file like `RedwoodWLAN.cpp`):
- **IOService named `bluetooth`** under the platform expert, with the properties on the service itself
  (`IORegistryEntrySearchCFProperty` checks the entry before recursing):
  - `local-mac-address`: 6 bytes, the loader's `mac-address-bluetooth0`, `xx:xx:xx:xx:xx:xx`
  - `transport-encoding`: u32 `1` (H4)
  - `transport-speed`: u32, the baud the kernel left the chip at
  
  This passes the gate in finding 1 (no `/dev` node needed) and gives the address.
- **Kernel control `com.apple.uart.bluetooth`** (`ctl_register`, *not* `CTL_FLAG_PRIVILEGED`, because both
  daemons run as `mobile`; `SOCK_STREAM`; one client at a time):
  - `ctl_send` → UART TX
  - UART RX (IRQ or a 1 ms poll thread) → `ctl_enqueuedata`
  - `ctl_getopt(TIOCGETA)` returns a termios with the current speed and `CS8|CRTSCTS`
  - `ctl_setopt(TIOCSETA)` is accepted. The kernel keeps its own baud, because the chip's baud is changed only
    with vendor command `0xfc18`.
  - Unknown options return `EINVAL`; the Skywalk options are never sent for this name.
- **Controller bring‑up in the kernel**, before the control is registered:
  1. BT_EN cycle and HCI_Reset at 115200
  2. patchram: `0xfc2e` Download Minidriver, then the `.hcd` records (each is a complete HCI command, mostly
     `0xfc4c` Write RAM), `0xfc4e` Launch RAM, 250 ms wait, HCI_Reset
  3. optionally `0xfc18` to 3 000 000 baud and reprogram UBRDIV
  4. `0xfc01` Write BD_ADDR = `local-mac-address`
  
  The `.hcd` comes from `/fw/` on the root, as `redwood_adsp.c` `root_file()` reads it: `bcm4375B1_semco.hcd`
  or `_murata`, chosen like the Wi‑Fi NVRAM.
- Optional: `/dev/btwake` (open = gpg1‑1 high, close = low). Without it bluetoothd only logs.

**Userland** (no Apple binary patched):
- `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth` for `com.apple.bluetoothd`. This is `mkjobs.py`'s
  existing `JOB_ENV` (`JOB_ENV="com.apple.bluetoothd:BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth"`; no
  `,baud` suffix, because JOB_ENV splits on commas), with `ALLOW="bluetooth"` to lift the DENY. The speed then
  comes from `transport-speed`, and the kernel keeps its own anyway.
- **A stand‑in `com.apple.BlueTool` job**: a ~60‑line `userland/src/bluetool_stub.c` listening on Mach service
  `com.apple.BlueTool`. It answers every `RunBuiltinScript` with `{result:0, manufacturer:15,
  firmware:"BCM4375B1 <hcd name>"}` and every `RunCommand` with `{result:0, output:""}`. The kernel has already
  booted the chip. A plist with the same label replaces Apple's, so stock BlueTool never runs.

**What bluetoothd sends first**, once the transport is open, is standard HCI over H4 (`01` + opcode + len): Reset,
Read Local Version, Read Local Supported Commands/Features, Read BD_ADDR, Read Buffer Size, LE set‑up. It needs
Command Complete/Status events (`04 0E …`/`04 0F …`) back in order, with flow control by
Num_HCI_Command_Packets. Apple‑specific vendor commands may follow. Any that fail are the next unknown, and they
are visible in bluetoothd's stderr (OSLOG).

Steps after B1/B2: **B3a** the IOService + kernel control (loopback‑testable with a 20‑line client), **B3b** the BlueTool
stand‑in + JOB_ENV, **B3c** bluetoothd up: Settings → Bluetooth toggles on and scans.

## Cellular: not planned

Shannon 5123 needs:
- a cpif shared‑memory/mailbox driver
- CP boot with the vendor `modem.bin` and a cbd‑like handshake, with no open precedent for this SoC
- iOS CommCenter, which speaks Qualcomm/Intel baseband protocols, not Shannon IPC

That is a multi‑month reverse‑engineering effort with low odds. Revisit only after everything else.

## W4/W5 as built (25 October): `wifi_pcie=4`, `bsd/dev/redwood_bcm4375.c`

Instead of LinuxKPI, a direct port of Linux 7.0.14 brcmfmac's PCIe path for this one chip, with the three BCM4375
patches (`xnu-deps/build/brcmfmac/src/brcmfmac`, the driver z3s runs this chip with). One thread, polled, every step logged:

1. **BARs:** BAR0 at the start of the outbound window (`0x11000000`), BAR2 at the next aligned address; the root
   port's bridge memory window; endpoint MEM + bus master. BAR0's 4 KB window (config `0x80`) onto the backplane.
2. **Chip:** chipcommon id (want `0x4375`), EROM walk (cores, revisions, wrappers); PCIe2 core rev → mailbox
   registers (≥ 64: int `0xC30`, mask `0xC34`, H2D `0xA20`/`0xA24`); ARM CR4 TCM size from its banks; RAM base `0x170000`.
3. **Firmware** from `/fw` (userland/build.sh ships `../fw-wifi`): `bcm4375-sta.bin` (`bcmdhd_sta.bin_b1`,
   18.41.117), NVRAM `bcm4375-nvram-semco.txt` (**SEMCO `CS01`, whose header says `Y2_EU`: this phone**; Murata with
   `wifi_nvram=1`), CLM `bcm4375-clm.blob`. ARM halted and D11 cores disabled, image at RAM base, RAM size from
   the image's `SMAR` field, NVRAM stripped and tokened at the top, reset vector to TCM 0, ARM released; up to
   5 s for the shared-area address in RAM's last word.
4. **Shared area** (version 5..7, `host_cap 0x00821407` for 7), the console (dumped to the log), the 5 common
   rings in the pool (TCM or DMA indices as the flags say), scratch + ringupd, HOSTRDY.
5. **msgbuf:** rx, event and ioctl-response buffers posted; ioctls through the control ring. `WLC_GET_VERSION`,
   `ver`, `cur_etheraddr`, CLM (`clmload` in 1400-byte chunks, `clmload_status`, `clmver`), `mpc 0`, `event_msgs`
   (SET_SSID, LINK, PSK_SUP, IF, ESCAN_RESULT), `WLC_UP`, `scan_ver`, then `escan` (v2 unless unsupported); every
   network heard is logged (BSSID, channel, RSSI, SSID).

**What the log says:** `wifi: chip id …`, `cores (N): …`, `W4 PASS: the firmware is up`, `console: RTE …`,
`shared area: …`, `ver: …`, `cur_etheraddr: …`, `CLM: …`, `W5 PASS: escan done …, N networks heard`, then a line a network.
**Next (W6):** `redwood_sta_ops` and `redwood_net_link` over this (scan/join through iOS's `wifid`, flow rings
for TX, RX completions to `en0`, WPA2 by the firmware's supplicant), then DHCP.

### Run 1 (25 October): panic in the BAR mapping, fixed

`kmem_alloc(..., 4194304, ...): failed with 3 @vm_kern.c:178` from `io_map()` ← `redwood_bcm4375_run`: arm's
`io_submap` is only 8 MB (osfmk/arm/io_map.c `IO_MAP_SIZE`); the 4 MB pool window plus the 4 MB BAR2 overflowed it.
BAR0/BAR2 and the pool window now come from `kernel_map` (`ml_io_map_unmappable`, as redwood_adsp.c does). Review
against brcmfmac/bcmdhd then added: the 128-byte host entropy below the NVRAM (`{count, 0xfeedc0de}`, Samsung's
`dhdpcie_wrt_rnd()`, brcmfmac's `WCC_SEED` for the 4375), bounds checks on every TCM address the firmware hands
back (ring info, ring memory, indices, console, mailbox words: a bad one would fault the kernel on device memory),
escan sizes as brcmfmac (v2 `length` 76, 84/76 bytes; v2 only when `scan_ver` answers), the device mailbox word
polled (deep-sleep ack only for PCIe2 rev < 64, as brcmfmac), shared-area ASSERT/TRAP flags on an ioctl timeout,
and the firmware's `cap` string logged.

### WPA3

The firmware does SAE itself (WCC: `brcmf_wcc_set_sae_pwd` → `WLC_SET_WSEC_PMK` with the passphrase flag; Samsung
builds `WL_SAE`). WPA3-Personal is then `wsec` AES + `wpa_auth` 0x40000 (`WPA3_AUTH_SAE_PSK`) + `mfp` 2 + the
password, if `cap` lists `sae`. Needs `REDWOOD_STA_WPA3_SAE` in redwood_sta.h and the Apple80211 side to advertise
SAE and hand over the password (WPA2 gets a PMK from wifid). WPA2/WPA3 transition networks join as WPA2 meanwhile.

### Run 3 (25 October): W4 PASS, W5 PASS

Firmware 18.41.117 up (shared area v7, TCM indices, 40 flow rings), MAC from OTP, CLM `9.9.5_Y2LSI` loaded, `WLC_UP`,
escan v2: 10 networks heard (2.4 and 5 GHz). Run 2 had failed only because the small image is mounted on /redwood:
firmware is now read from the RAM disk (`redwood_image_file("fw", …)`), as the audio and GPU drivers do.

### W6 (built, run 4): station + en0

`redwood_bcm4375.c` stays after the scan: `redwood_sta_ops` (power WLC_UP/DOWN; scan = escan with name/channels/
passive/dwell, BSSes kept with their IEs; join = wsec AES + wpa_auth WPA2-PSK + auth open + `sup_wpa 1` +
WLC_SET_WSEC_PMK (passphrase, or PMK) + `join` (WLC_SET_SSID fallback), joined on E_LINK up + E_PSK_SUP 6, then flow
ring 2 created; leave = WLC_DISASSOC + flow ring delete) and `redwood_net_link` (TX: mbuf copied into one of 64 pool
buffers, TX_POST on the flow ring; RX: RX_CMPLT frames to `redwood_net_input()` after the ring lock, buffers
re-posted; `poll` = en0's thread). Locks: ring_lock (rings/buffers), ioctl_lock, poll_lock (one poller), a spin
lock for `state()`. Lost link (deauth/disassoc/E_LINK down while joined) → en0 down + `redwood_sta_lost()`.
`wifi_pcie_wait=3` so en0 exists before wifid (~25 s); `net_test wifi` treats `wifi_pcie>=4` like `wifi=1`;
`fw-wifi/network` (name, passphrase; gitignored) is joined at boot if present; `RC_JOBS=rc.jobs.wifi` waits 3 min
for an address, then DNS, TCP and HTTPS.

### WPA3 (built, not yet flashed)

`sta_join()` reads the AP's RSN element (AKM suites, RSN capabilities): WPA2-PSK wherever PSK is offered (WPA2/WPA3
transition networks too), SAE where it is the only one (`wifi_sae=1`: SAE wherever offered): `auth` 3,
`wpa_auth` 0x40000, `mfp` 2, `sup_wpa` 1, the passphrase by WLC_SET_WSEC_PMK with WSEC_PASSPHRASE (brcmfmac
WCC's `brcmf_wcc_set_sae_pwd`). The W5 scan log names each network's security. iOS side: wifid refuses to add a
WPA3-only network unless the driver says it can ("WPA3 not supported, will not add WPA3-only network"); which
capability bit that is (not in AirportItlwm's apple80211_var.h, bits 0..21) is still to be read out of IO80211.

### Run 4 (25 October): W6 up, SpringBoard crash loop (Siri), quiesce panic

en0 attached, wifid talking (CURRENT_NETWORK, 383 refused), firmware `cap` has `sae mfp`. SpringBoard aborted every
~5.7 s from 84 s: SpringBoard → SiriUIActivation → SiriUICore → MPSImage → MPSCore → Metal (MTLReportFailure,
abort) -- MPS rejects our Metal device; Siri's UI is set up now that there is a network interface (likely). Fix:
`Assistant Enabled = false` in userland/var/mobile/Library/Preferences/com.apple.assistant.support.plist.
Vol-Down+Power then panicked in `redwood_wifi_quiesce()` → `redwood_apple80211_summary()` → printf with interrupts
off: skipped there now. Run 5 = these + WPA3 auto-select.

### Run 6 (25 October): Settings lists networks, join refused: no firmware supplicant

SpringBoard crash loop gone: `inject_sb.c` sets Metal's error mode 4 for failure type 0
(`MTLFailureTypeSetErrorModeType(0, 4)`), so `-[MPSKernel initWithDevice:]` returns nil instead of aborting
("device is not supported"). Wi-Fi in Settings, networks listed, password taken; wifid ASSOCIATE gives a PMK
(kind 6, 32 bytes). "the test access point" is WPA2-PSK (AKM 0x4, RSN caps 0xc). `sup_wpa` → -23 (BCME_UNSUPPORTED):
Samsung's firmware has no supplicant (`cap` has no `idsup`; Android's wpa_supplicant does it).

### Run 7: the host's WPA2 supplicant

lx_wpa.c (the repository's own four-way handshake arithmetic, wifi/lx) compiled into the kernel (bsd/dev/lx_wpa.c,
`KERNEL` guard). On E_LINK up: flow ring, the firmware's RSN element from `assoc_info`/`assoc_req_ies` for message 2,
EAPOL (0x888e) frames taken out of the RX path into a queue, answered (`eapol_handle()`, never from inside an ioctl),
message 4 let out (TX status, ≤300 ms) before `wsec_key` gets the pairwise key (algo AES_CCM, ea = AP) and the group
key (primary, rxiv from the RSC); group rekeys later the same way. 10 s for the handshake.

### Run 9 (25 October): WPA2 handshake done by the host, data frames in -- no DHCP

RX was silent until the mailbox mask was enabled (brcmf_pcie_intr_enable: 0xffff on PCIe2 rev 64) and 256 RX buffers
posted. Then: EAPOL M1 → M2, M3 → M4, pairwise + group keys by `wsec_key` (both 0), `W6 PASS: joined (-50 dBm, 5180
MHz)`, ASSOCIATE_RESULT 1 to wifid; IPv4/IPv6 multicast (mDNS) frames reach the stack. But en0 stays flags 0x8802
(never IFF_UP): nothing sent, no DHCP; Settings spins. en0 attaches ~30 s, after configd (27.5 s) -- the ath11k path
makes en0 at kernel start for this reason. Run 10: configd's os_log on, configd's store dumped, and `net_test dhcp
en0` a minute after the link if configd has not configured it.

### Run 10 (25 October): INTERNET OVER WI-FI

Joined from Settings, WPA2 by the host supplicant, Safari loaded google.com and apple.com. net_test: example.com and
apple.com/library/test/success.html both 200 over HTTPS; 44 MB in, 1.7 MB out, nothing dropped in. Over IPv6 only:
SLAAC addresses (a global IPv6 address), no IPv4 -- configd had no network service for en0 (`Setup:/Network/Service/*/Interface`:
0 keys), and `net_test dhcp en0` was asked before the join and given 40 s. (net_test had not been rebuilt: build.sh
copies prebuilt/userland/ when ld64 is not on PATH -- now rebuilt into prebuilt/.)
Run 11: en0 attached right after setup() (before the boot escan, scans serialized) to beat configd's start; the jobs
wait for the link, then 30 s for configd's address, then ask DHCP themselves.
