# 09 — iOS as the only OS: boot menu, flashing and logs without Android, iOS on the flash

Goal (25 October): the S20+ boots straight into our loader; a menu offers **Boot iOS**, **Log access**,
**Download Mode**; kernels and images are flashed and logs pulled **without Android**; then iOS lives on the
phone's 122 GB UFS instead of in RAM. Samsung's own Download Mode (S-Boot) is never touched and always stays
reachable (Vol-Down + Power, then Vol-Up; or from the menu).

## What the phone already has (found 25 October)

| Need | Already there | Where |
|---|---|---|
| Read/write the UFS | UEFI `UfsDxe` (BlockIo); the loader reads `recovery`/`super` and writes its log with it | `Mu-Silicium/Silicon/Samsung/SamsungPkg/Drivers/UfsDxe`, `xnuloader/loader.c` |
| USB device | UEFI `UsbfnDwc3Dxe` = standard `EFI_USBFN_IO_PROTOCOL` (DWC3, ported from Samsung's 9830 kernel) | `SamsungPkg/Drivers/UsbfnDwc3Dxe`, in y2s.fdf |
| Keys | UEFI `KeypadDxe`: Vol-Up = SCAN_UP, Vol-Down = SCAN_DOWN, Power = Enter | `y2sPkg/Library/KeypadDeviceLib` |
| A fastboot server | the loader's `fb_command()` (download, upload, log, reboot) — over Qualcomm's USB protocol only | `loader.c` |
| Reboot to Download Mode | Samsung: PMU INFORM2[23:16] = 0x4E, INFORM3 = 0x12345601, PMU SWRESET | `drivers/samsung/sec_reboot.c`, `exynos-reboot.c` (PMU 0x15860000, mapped in UEFI) |
| Flashing from Download Mode | heimdall flashes any partition (MISC already done this way) | `heimdall-arm64` |
| Return from iOS to the loader | Vol-Down + Power 3 s → the loader saves the log | `loader_return`, `write_logs()` |

## Disk (LUN 0, 122 GB)

`boot` 59 MB @121.6 MB · `recovery` 65.8 MB @180.6 MB (our "disk": boot image, log @16 MB, kernel @32 MB,
root.apfs @48 MB) · `super` 9000 MB @404 MB (system.apfs at +0x184B00000) · `prism` 1000 MB · `cache` 600 MB ·
`userdata` 110.9 GB @11134 MB. EFS (IMEI, radio calibration) in `efs`/`sec_efs`; bootloaders in LUNs sdb/sdc.

## Phases

- **P0 — backups (done 25 October).** Every partition but super/userdata/prism/cache, the four boot LUNs and
  every GPT head: `backup-y2s/` (39 files, MD5SUMS, verified against the phone).
- **P1 — the boot menu (loader).** Drawn on the panel with XNU's 8x16 console font scaled up; Vol-Up/Down move,
  Power selects; **Boot iOS** after 3 s untouched. Entries: Boot iOS · Log access (USB) · Download Mode ·
  Reboot · Power off. Shown at power-on and again when iOS returns (instead of "holding").
- **P2 — Log access over USB.** The fastboot server on `EFI_USBFN_IO_PROTOCOL`; `oem lastlog` stages the
  last run's log from `recovery`+16 MB; `oem write <part> <hexoff>` writes what was downloaded (whitelisted:
  boot, recovery, super ≥ 0x184B00000, userdata) and reads it back. Host: `y2s.sh ulog`, `uflash`, `uboot`
  (stock `fastboot`, VID 0x18d1).
- **P3 — Android off the boot path.** The boot image (BootShim + UEFI + payload, 2.4 MB) into `boot`: power-on
  goes to the menu; `recovery` stays the disk. Android is back with `heimdall flash --BOOT backup-y2s/boot.img`.
  Gate: P1 + P2 work on the phone.
- **P4 — logs over Wi-Fi while iOS runs**: a small daemon serving the live kernel log (HTTP), `y2s.sh wlog`.
- **P5 — iOS from the flash.** An Exynos UFS path in XNU (the `redwood_ufshc.c` design: borrow UEFI's
  initialised UFSHCI at 0x13100000, own transfer list; Exynos quirks from Samsung's `ufs-exynos.c`), the iOS
  volume in `userdata` (`oem write`), root from flash (init's `root_apfs=` mode): 2.3 GB of RAM back, the whole
  IPSW volume. Gate: the user agrees to erase Android's `userdata`.
- **P6 — persistent writable data** (`/var`): APFS allocation in `apfs_write.c`, or a second filesystem.

## Safety rules

- Never write sdb/sdc (bootloaders), `efs`, `sec_efs`, `param`, `up_param`, `misc` (except the stock file),
  `vbmeta*`, `keydata`, `keyrefuge`, `steady`, `persistent`. The loader's `oem write` refuses all of them.
- `by-name/sda` is the WHOLE disk on this phone: never script over `by-name/*` without excluding sda..sde.

## Built 25 October (run S1, flashed to `recovery` over adb; Android still on `boot`)

- `xnuloader`: `y2s_menu_loop()` (efi_main on y2s): HackiOS menu drawn with XNU's 8x16 font (`menu_font.h`,
  generated from `osfmk/console/iso_font.c`), keys from `ST->con_in`; Boot iOS after 3 s untouched at power-on,
  waits after a run. Download Mode = PMU INFORM2/INFORM3 + SWRESET; Power off = EfiResetShutdown, then PS_HOLD.
- USB: `EFI_USBFN_IO_PROTOCOL` client (`ufn_*`: descriptors on ep0, bulk ep1 512) under the existing fastboot
  server; `stage_buf` 96 MB; new commands `oem lastlog`, `oem write PART HEXOFF` (recovery, boot, super from
  0x184B00000; read back), `oem boot`, `oem download-mode`.
- Kernel: `redwood_fb_quiesce()` from `loader_return()`: the panel's window back on S-Boot's channel.
- Host: `y2s.sh ulog | uflash | uboot | hflash | standalone`.

## Run S1/S2 results (25 October)

- Menu drawn, keys work, Log access enumerates on the Mac (`fastboot devices`: xnuloader). **Download Mode from
  the menu works.** `y2s.sh hflash` (heimdall --RECOVERY from Download Mode) works and reads back identical.
- USB bug: only the first fastboot session answered. macOS's fastboot clears both bulk pipes at every open
  (CLEAR_FEATURE ENDPOINT_HALT): the host's data toggle goes to DATA0, the device's did not. Now forwarded to the
  driver (DWC3 CLEARSTALL resets the sequence); sends time out after 5 s. Then: repeated sessions fine,
  `ulog` 8 MB in ~1 s, `uflash` 62 MB written + read back in ~2 s (in two pieces around the log at 16..32 MB, so a
  flash keeps the last log), `uboot` starts iOS.
- After heimdall's reboot the phone boots Android: S-Boot takes `boot` unless told otherwise -- P3 fixes that.

## P1-P3 done (25 October)

- Return from iOS (Vol-Down + Power 3 s) shows the menu ("iOS returned: its log is saved"): `redwood_fb_quiesce`
  works. `ulog` after a run: 1.7 MB of log over USB.
- **P3:** `y2s.sh standalone` wrote the boot image (2.4 MB, ANDROID! header: BootShim + UEFI + payload) into `boot`,
  read back. Proof: `fastboot oem download-mode` → heimdall writes stock `misc` (no recovery flag) → plain reboot →
  the loader answers on USB, no adb. **Power-on now goes to the HackiOS menu; Android no longer boots.**
  Undo: Download Mode, `heimdall flash --BOOT backup-y2s/boot.img`.
- The loop without Android: menu → Log access → `./y2s.sh uflash` (recovery, and boot too: out/.standalone) /
  `ulog` / `uboot`; or Download Mode (menu, or `fastboot oem download-mode`) → `./y2s.sh hflash`.

## P4 + P5a built (25 October, run S3)

**P4 — logs over Wi-Fi.** The kernel's log ring (the same text the loader saves on return) is readable
while iOS runs: `sysctl kern.redwood_log` (root) and `kern.redwood_log_total` (characters ever written),
in `bsd/dev/redwood_logd.c` over `loader_log_snapshot()` (`osfmk/arm64/loader_return.c`). The launchd job
`com.redwood.logd` runs `userland/src/rwlogd.c`: HTTP on port 8080 (`/log`, `/tail?from=N` with an
`X-Total` header), Bonjour name "HackiOS" (`_http._tcp`), and its addresses printed in the log. On the Mac:
`./y2s.sh wlog` (whole log, also `../xnulog.txt`) and `./y2s.sh wtail` (follow); `Y2S_IP=a.b.c.d` skips
Bonjour.

**P5a — the kernel's own UFS driver on the Exynos 990, read only.** `redwood_ufshc.c` takes the controller
at 0x13100000 with the Exynos quirks (from Samsung's `ufs-exynos.c` and Silicium's UfsDxe): response/PRDT
offsets and lengths in bytes, PRD entries of at most 4 KB, UTRLCLR clears with ones, and every slot's bit in
`UTRL_NEXUS_TYPE` (vendor 0x1140) must say SCSI command (it refuses otherwise). Everything else of the
Xiaomi design stands: the whitelist (reads, TEST UNIT READY, no write op, no W flag, no well-known LUN), the
proof in slot 0, the list in the loader's memory, UEFI's list put back on return (kernel quiesce and the
loader's `hc_take_back`). As the kernel runs on the loader's core here, the loader serves nothing while it
runs (`native.flags` `BLOCK_NATIVE_ONLY`): it reads each unit's GPT header into its buffer first, which the
kernel uses to tell the LUNs apart; reads never fall back to the loader.
`ufs=2 ufs_native=1` gives `/dev/ufs_NAME` / `/dev/rufs_NAME` for every partition. `ufs_native_md5=1` hashes
untouched partitions, LUNs 1-4 and the system image in super (+0x184b00000, 0x948c0000 bytes) in a thread
of its own (`ufs: md5:` lines), for comparison with `backup-y2s/MD5SUMS` and `userland/system.apfs`.

### Run S3 findings (25 October)
- P4 verified: `./y2s.sh wlog` fetched the whole log over Wi-Fi (Bonjour may be slow to resolve; `Y2S_IP=address`
  works). The console's log alone lacked every plain `printf` (they go only to the message buffer, which keeps about
  the last half minute), so `kern.redwood_dmesg` (+ `_total`, counted in `log_putc_locked`) was added and `/log` now
  gives both, as the loader does on return; `/tail?log=N&dmesg=M` follows both.
- Wi-Fi joins by itself only with `fw-wifi/network` (two lines: name, passphrase; git-ignored, 0600).
- UEFI's (Samsung UfsDxe) command descriptor is in a page of its own (list eb547000, UCD eb53d000): mapped apart.
- The loader's log read-back reuses `block_buffer`: the units' GPT headers for the kernel are in `block_gpt` now.
- LUNs 1, 2, 4 have no GPT: in native-only mode they are skipped (no read goes to them), not a failure.
- The slots' buffers are no longer `kmem_alloc_contig` on Exynos (PRD entries are 4 KB anyway): one run with them
  had no GPU (Mali's bring-up printed nothing; backboardd exited "Metal failed to build render pipeline").
- The kernel's uptime includes the time spent in the loader's menu before the boot: not a measure of boot time.
- `ufs:` messages also go to the console's log now (they were lost from the message buffer by the time of a pull).

### P5a works (run S3, 25 October, 22:35)
The kernel's own Exynos UFS driver reads the flash. **The S20+'s controller does not take a doorbell bit back
while IS's UTRCS is set**: a command finishes (data in, response and OCS 0 written) but its UTRLDBR bit stays
until UTRCS is cleared (UEFI's UfsDxe clears IS after every command, which is why it never sees this). The
first command after the switch works only because UEFI left IS clear. `hc_finished()` takes a slot as done when
its doorbell bit is clear or its OCS is no longer the invalid one written; `hc_retire()` clears UTRCS (and the
slot's UTRLCNR bit), and UTRLCLR (a one) only if that does not do it. In the md5 run: 2715 slots taken back by
the controller, 7843 by the acknowledgement, 0 cleared, 0 stuck.
`ufs_native_md5=1`: **all 13 reads match** `backup-y2s` (radio, optics, spu, omr, bota, dtbo, keystorage, uh,
harx, vbmeta, blcmd, LUN 3 whole = lun-sdd.img) **and the system image in super = userland/system.apfs**
(7c2023fa…, 2.49 GB at 96 MB/s, one read at a time, md5 in the loop). Data buffers are in the loader's 4 MB
buffer (16 slots × 256 KB, low memory). Whether kernel memory above 4 GB works for data is not yet known:
the earlier stuck reads into it were most likely this retirement issue, not DMA reach -- to be tried for speed.
Other findings: boot options and the loader change only after **Reboot** in the menu (`uflash` says so);
the up_param splash (`./y2s.sh hlogo`, `out/up_param-apple.img`) is prepared.

### P5b works: iOS's root from the flash (25 October, 22:45)
`ufs_root=super:184b00000:948c0000` (with `ufs=2 ufs_native=1`): the loader no longer reads the 2.3 GB system
image into memory; the payload's small image is the only RAM disk (`RAMDisk1` → md1, `mdevadd(1, …)`), and
`rd=md0` stays on the command line for launchd and `bsd_rooted_ramdisk()`. `IOFindBSDRoot` sees `ufs_root=`,
starts the UFS driver early (`redwood_ufs_root()`; `redwood_ufs_init()` runs once) and roots on
`/dev/ufs_root`, a read-only window of super (bsd/dev/redwood_ufs.c). First run: root mounted, SpringBoard up,
Wi-Fi log fine, no read errors; available memory 3136 MB against 1629 MB in the RAM boot (wired 2793 vs 3346).
Undo: the payload without `ufs_root=` (the image is still loaded from super as before).
Host: `./y2s.sh ureboot` restarts the phone from Log access (for loader/command-line changes).

### P6 built: the data volume kept in userdata (25 October, 23:00; the user chose userdata over cache)
- **Store**: `userdata` (LUN 0, LBA 2850304, 28382208 blocks: checked before anything is allowed), 64 MiB in,
  two slots of 2 GiB (`userland/src/var_store.c`, `pick_layout`: `/dev/rufs_userdata` when there is no darwin).
  Android's data in that window is overwritten at the first save; the rest of userdata is untouched for now.
- **Kernel writes** (`redwood_ufshc.c`): WRITE(10) and SYNCHRONIZE CACHE(10) only through `hc_write_refusal`,
  which allows LUN 0 blocks `[2850304+16384, +1048576)` and nothing else (`ufshc_set_write_window`, once). The
  read-only list (`hc_refusal`) is unchanged and still refuses every write; a self-test of the write checks runs
  at start on a made-up window. `redwood_ufs.c`: each 4 MB written is read back and compared; the last close of
  the device sends SYNCHRONIZE CACHE.
- **When it saves**: on Vol-Down+Power (the keys thread asks the saver and waits, `var_save_ms=90000`), and on
  iOS's Restart / Shut Down: launchd's SIGTERM to the registered saver is turned into a save request
  (`bsd/kern/kern_sig.c` → `redwood_var_saver_sigterm`; the plist's ExitTimeOut is 120 s), and `halt_all_cpus`
  now returns to the loader's menu. No periodic save (the user's choice).
- **At boot**: init's `restore_var` (var_store=1) unpacks the newest good slot; none or a bad one: a clean /var.
- Command line: `... ufs=2 ufs_native=1 ufs_root=super:184b00000:948c0000 ufs_write=1 var_store=1 var_save_ms=90000`.
- `var_store` is now in `prebuilt/userland` (it was missing: its job exited 78 at every boot).

### P6, done properly: /private/var is a real HFS+ on userdata (25 October, 23:35)
The var_store archive (above) was a checkpoint save; replaced by a writable file system, as a phone has:
- **HFS+ in the kernel**: Apple's own (apple-oss-distributions/hfs, tag hfs-627.100.6 -- the macOS 13 one, matching
  xnu-8796), `core/` copied to `bsd/hfs/` and built in with the kext's view of the headers (`bsd/conf/Makefile.template`:
  `-DPRIVATE`, the prefix header `bsd/hfs/kext-config.h`, `-DXNU_KERNEL_PRIVATE -DBSD_KERNEL_PRIVATE` removed; the
  SDK's `TargetConditionals.h` beside it). No content protection (no AppleKeyStore), no quotas; the IOKit glue is
  `bsd/hfs/hfs_redwood.c` (`redwood_hfs_init`: `vfs_fsadd` as `com_apple_filesystems_hfs::start` did). The iOS
  build of HFS does not use hfs_encodings. The kernel is 16.63 MB of its 16.78 MB slot.
- **The volume**: case-sensitive journaled HFS+ (HFSX, 4 KB blocks) filling userdata (28382208 blocks), made by
  macOS's `newfs_hfs` on a sparse image (`tools/mk-userdata-hfs.sh`, run by the user: the tool may not create disk
  images) and reduced by `tools/hfs-kit.py` to a 60 KB kit: the 54118 blocks the file system uses, 15 of data and
  211 MB of zeros (`userland/hfs-kit.bin` → `/redwood/share/hfs-kit.bin`).
- **The kernel's writes**: the window is now all of userdata (and nothing else); `/dev/ufs_userdata` may be opened
  for writing, its strategy writes through `ufs_write_direct` (buffers mapped with `buf_map`), DKIOCSYNCHRONIZE is
  SYNCHRONIZE CACHE, DKIOCISWRITABLE says so. The keys' way out syncs every writable local file system with
  MNT_WAIT and flushes the unit's cache (`redwood_sync_filesystems`); iOS's Restart/Shut Down go through
  reboot_kernel's own sync and unmount, then back to the loader's menu.
- **init** (`var_hfs=1`): no HFS+ on userdata ("HX", version 5 at 1024) → the kit written (each write read back);
  then `mount("hfs", "/private/var", 0, {"/dev/ufs_userdata", ...})`; the skeleton keeps what is there (existing
  directories and files left alone); `/private/var/run` emptied. Anything failing: the memory filesystem, as before.
  init is rebuilt from source (the prebuilt one matched the source: same size, same strings).
- Also (found by the first var_store save): the S20+'s controller left with IS.UTRCS set makes UEFI's next command
  hang; the kernel acknowledges it after every command and at hand-back, and the loader clears it on take-back.
- **Verified on the phone (23:40)**: first boot made the volume from the kit (211 MB written and read back); a first
  mount panicked in `zalloc` (HFS's zones were never made: `hfs_init_zones`/`hfs_sysctl_register` were missing from
  `redwood_hfs_init`; fixed); then "hfs: mounted Data on device ufs_userdata", the journal initialised, and the
  user's changes survived a return to the menu and a new boot. Not yet tried: iOS's own Restart / Shut Down.
- **M5 cores (25-26 October)**: Safari's WebContent crashed (SIGSEGV/SIGILL) at every page with cpumask=0xff dvfs_cl2=2304000; with cpumask=0x3f (no M5s) it works. The per-core register dump (cpu-ident:) is in this build's log; cause (cache-type mismatch vs voltage) still to be pinned. Running without the M5s for now.
