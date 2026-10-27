# Building

This page is the long form of the build section in the [README](../README.md). The repository is a snapshot of the port and expects the upstream HackiOS trees to be present. It is not a standalone clone.

## What is assumed

The snapshot was taken in October 2026 against:

- `hackios-xnu-kernel`, the XNU patch set, loader, UEFI scripts and build system.
- `hackios-xnu-userland`, the init, injected libraries, launchd jobs and APFS image builder.
- `Project-Silicium/Mu-Silicium`, the UEFI.
- Apple `xnu-8796.121.2` from `apple-oss-distributions/xnu`, fetched by `xnu/apply.sh`.
- Mesa 25.0.7 for the GPU userland.

Upstream changes after that date may not apply cleanly. The kernel overlay is small enough to port by hand if a hunk rejects; the UEFI patch is four files.

## Host requirements

### macOS

The kernel and userland build natively on Apple silicon. This is the tested path.

```sh
xcode-select --install
brew install coreutils gnu-tar gnu-sed findutils bison qemu sevenzip lz4 dtc
```

Theos and the iPhoneOS 16.5 SDK:

```sh
mkdir -p ~/theos
git clone --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks ~/theos/sdks
git -C ~/theos/sdks sparse-checkout set iPhoneOS16.5.sdk
```

Add the project's macOS environment helper to your shell:

```sh
echo 'source /path/to/hackios-xnu-kernel/common/macos-env.sh' >> ~/.zshrc
```

The helper puts GNU `stat`, `sed`, `tar` and the project's shims on `PATH`. XNU's makefiles expect GNU tools and the BSD versions on macOS are not compatible.

### Linux for the UEFI

Mu-Basetools has no macOS host support. Use an arm64 Ubuntu machine or a Lima VM:

```sh
limactl start --name=hackios --arch=aarch64 --memory=8 --cpus=4 template:ubuntu
limactl shell hackios
```

Inside the VM, follow Mu-Silicium's `setup_env.sh`. The port used a Python virtual environment named `venv-linux` inside the Mu-Silicium tree.

## Assembling the tree

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git
cd hackios-xnu-kernel
common/setup.sh
```

`common/setup.sh` brings `hackios-xnu-userland` into the same tree, checks the host, and fetches the dependencies. Then overlay this snapshot:

```sh
SNAP=/path/to/y2sios

rsync -a "$SNAP/kernel/xnu/" xnu/
rsync -a "$SNAP/kernel/xnuloader/" xnuloader/
rsync -a "$SNAP/kernel/common/" common/
rsync -a "$SNAP/kernel/tools/" tools/
rsync -a "$SNAP/userland/src/" userland/src/
cp "$SNAP/kernel/y2s.sh" "$SNAP/kernel/m2m.cmdline" .
cp "$SNAP/userland/build-gpu-y2s.sh" "$SNAP/userland/build.sh" \
   "$SNAP/userland/mkjobs.py" "$SNAP/userland/mkeagl.py" userland/
cp "$SNAP/userland"/rc.* userland/
cp "$SNAP/userland"/*.plist userland/
cp "$SNAP/userland/hfs-kit.bin" "$SNAP/userland/m2m.tour" userland/
rsync -a "$SNAP/userland/libmini/" userland/libmini/
```

The analysis tools (`tools/dsc*.py`) need `pip3 install --user capstone` and the extracted shared cache in `ios165/`.

The `xnu/` directory now contains the y2s overlay: `redwood.patch`, `files/`, `apply.sh` and `export.sh`. If the upstream tree already has a different `xnu/redwood.patch`, our copy replaces it. That is intentional; this snapshot is the complete kernel state.

## Kernel

```sh
xnu/apply.sh
xnu-deps/setup.sh

. common/macos-env.sh
export GIC_V2=1
./xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k
```

Or, in one step:

```sh
./y2s.sh kernel
```

`y2s.sh kernel` sources the environment, exports `GIC_V2=1`, builds, refuses to continue on a compiler warning or error in the project's own files, checks that the binary contains the string `gic: v2`, copies it to `out/mach.y2s`, and runs `xnu/export.sh`.

The build ends with a non-zero exit status. This is expected. XNU's `check_all_exports` step complains about symbolsets and DriverKit RPC names, but the kernel has already linked. Look at the file, not the status.

If the kernel crashes immediately after `pe_arm_init_debug`, the build is a GICv3 kernel. Confirm with:

```sh
strings out/mach.y2s | grep 'gic: v'
```

It must print `gic: v2`. If it does not, `GIC_V2` was not exported into the child `xnu-build.sh`. Touching the dependent objects or removing `BUILD/obj/DEVELOPMENT_ARM64_BCM2837` forces a rebuild after a `-D` change, because make does not notice a changed compiler flag on its own.

The kernel must fit in 16 MiB. `y2s.sh kernel` refuses a larger one.

## Loader

```sh
GIC_V2=1 xnuloader/build.sh
```

The loader is a freestanding AArch64 UEFI application linked at `0xA1000000`. `GIC_V2=1` is required for the same reason as the kernel: a GICv3 loader takes a UEFI exception when it reads `ICC_PMR_EL1` on the way into the kernel.

`./y2s.sh payload` builds the loader and the XNUP container, then splices the container into `out/y2s-embed.img` at `0x100870`. The payload lives inside the UEFI firmware volume's free tail. Do not append it after the image.

## UEFI

```sh
git clone --recursive https://github.com/Project-Silicium/Mu-Silicium.git
cd Mu-Silicium
git apply /path/to/y2sios/uefi/y2s.patch
```

The patch changes `BootShim/BootShim.S`, `BootManager.c` and `BootManagerLib.inf`. Copy the new files:

```sh
cp -a /path/to/y2sios/uefi/Platforms/Samsung/y2sPkg Platforms/Samsung/
cp /path/to/y2sios/uefi/Resources/Configs/y2s.toml Resources/Configs/
cp /path/to/y2sios/uefi/Resources/DTBs/y2s.dtb Resources/DTBs/
cp /path/to/y2sios/uefi/BootManagerLib/XnuLoader.c \
   /path/to/y2sios/uefi/BootManagerLib/XnuLoader.h \
   /path/to/y2sios/uefi/BootManagerLib/BootManager.c \
   /path/to/y2sios/uefi/BootManagerLib/BootManagerLib.inf \
   Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/
```

`y2s.dsc` and `y2s.fdf` point at `x1s/AcpiTables.inf` from the `Silicium-ACPI` submodule. The tables are inert, but `AcpiTableDxe` must exist or BDS will not dispatch.

Build inside the Linux VM:

```sh
python3 -m venv venv-linux
venv-linux/bin/pip install -r pip-requirements.txt
cd Mu-Silicium
source venv-linux/bin/activate
python3 build_uefi.py -d y2s
```

Output:

```
Build/y2sPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd
```

It should be exactly 2 MiB. `out/y2s-embed.img` is a prebuilt Mu-x1s boot image shell with this firmware volume spliced in at `page + 112`.

The Mu-Basetools `mu_nasm` dependency is installed through NuGet and expects Mono. If Mono is not available, place the system `nasm` in the expected directory:

```sh
D=Mu_Basecore/BaseTools/Bin/mu_nasm_extdep
mkdir -p $D/Linux-x86-64
echo 'version: 20016.1.1' > $D/extdep_state.yaml
ln -s /usr/bin/nasm /usr/bin/ndisasm $D/Linux-x86-64/
```

## iOS user space

`ios165/fetch.sh` downloads the iOS 16.5 IPSW from Apple's CDN and extracts what is needed into `ios165/`. The image is about 7.4 GB and extraction takes 5 to 15 minutes. No Apple files are in this repository.

```sh
cd userland
../ios165/fetch.sh
```

The userland build needs:

- theos with `iPhoneOS16.5.sdk`;
- `ld64` and the cctools from theos, or the macOS `xnu-clang` shim;
- Mesa built with the panfrost driver for `build-gpu-y2s.sh`;
- Python 3 and `tools/apfs/mkapfs.py`.

Build the GPU pieces and the images:

```sh
QUIET=1 RC=rc.launchd.metal RC_JOBS=rc.jobs.manual ./build-gpu-y2s.sh
```

`RC=rc.launchd.metal` sets up the Metal plug-in before the window server starts; without it CoreAnimation uses OpenGL ES.

The script builds Mesa into `xnu-deps/build/mesa/out`, builds `libOSMesa.8.dylib`, `libredwood-gl.dylib`, the Metal libraries, the tests and `init`, then calls `build.sh` with `GL=1`.

`build.sh` produces:

- `userland/root.apfs`, the 4 MiB small image mounted on `/redwood`.
- `userland/system.apfs`, the 2.3 GB iOS root image.

The system image can be rebuilt with `SYSTEM=1`. It is not committed.

For a boot that roots from the flash and keeps `/var` on `userdata`, use `m2m.cmdline`, the standard command line shown in the README. The first boot creates the HFS+ volume from `hfs-kit.bin`; later boots mount it.

## Firmware

The phone's own firmware is required. It is not redistributable here. Extract it from the stock AP tar for the exact model and region:

1. Download the stock AP tar for SM-G986B.
2. Parse `super.img.lz4` and the vendor partition. `reference/firmware-notes.md` has the commands and the full file list.
3. Copy the BCM4375 firmware, CLM and NVRAM files into `fw-wifi/` with the names `build.sh` expects:
   - `bcm4375-sta.bin` (from `bcmdhd_sta.bin_b1`)
   - `bcm4375-clm.blob`
   - `bcm4375-nvram-semco.txt` (or the Murata variant)
   - `bcm4375B1-semco.hcd`
4. Put the network name and passphrase in `fw-wifi/network`, two lines, mode 0600. It is git-ignored.

The SEMCO NVRAM is the right one for this phone; its header says `Y2_EU`. The Murata variant can be selected with `wifi_nvram=1` if the chip reports a Murata module.

The audio path does not need the DSP firmware on the first attempt, because the AP programs the ABOX directly. The amplifier tuning files are only needed if the DSP speaker-protection path is used.

## Pack

```sh
./y2s.sh payload "$(cat m2m.cmdline)"
./y2s.sh pack
```

With iOS already running on Wi-Fi, `./y2s.sh loop LABEL` builds, flashes, boots, runs the standard tour and reports the m2m channel against the previous run.

`payload` rebuilds the loader and the XNUP container, reads the exact size of `root.apfs`, and splices the container into `out/y2s-embed.img`. If `root.apfs` changes size, run `payload` again before `pack`. The loader reads exactly the size recorded in the payload; a mismatch produces an APFS error and a halt on the Silicium screen.

`pack` writes:

```
out/y2s-recovery.img = embed @ 0
                     + kernel @ 32 MiB
                     + root.apfs @ 48 MiB
```

The image is checked against the 65.8 MiB size of the `recovery` partition.

## Flash

### From Android

```sh
./y2s.sh flash
```

This runs `adb root`, pushes the image, writes it with `dd`, reads the same region back and compares the md5, then runs `adb reboot recovery`. `adb root` is lost on every reboot, so the script repeats it.

### From the loader menu

Boot the phone into the menu, choose **Log access (USB)**, and connect it:

```sh
./y2s.sh uflash
```

The image is written in two pieces around the 16 MiB log region, so the previous run's log survives. The phone reads the data back and verifies it.

### From Download Mode

```sh
./y2s.sh hflash
```

This uses the arm64 Heimdall build. It writes `recovery`, and `boot` as well if `out/.standalone` exists.

### One-time standalone setup

```sh
./y2s.sh standalone
```

This writes the boot image into `boot`, so power-on goes to the HackiOS menu and Android no longer starts. Android's data is untouched. Undo with:

```sh
heimdall flash --BOOT backup-y2s/boot.img
```

## Logs

```sh
./y2s.sh log      # from Android, after a return or a panic
./y2s.sh ulog     # over USB from Log access
./y2s.sh wlog     # over Wi-Fi while iOS runs
./y2s.sh wtail    # follow the Wi-Fi log
```

`wlog` uses Bonjour to find `HackiOS._http._tcp`. If Bonjour is slow, set `Y2S_IP=address`.

```sh
./y2s.sh m2m fetch --label NAME   # the m2m channel into logs/m2m/
./y2s.sh m2m report RUN           # what a run says, compared with the run before
python3 tools/frame_split.py LOG  # per-frame timing from a touch_lat=1 log
```

## Common failures

| Symptom | Likely cause |
|---|---|
| Black screen, then Download Mode | ACPI removed from the UEFI, or the payload was appended after the firmware volume |
| Kernel dies silently after `pe_arm_init_debug` | GICv3 kernel; `GIC_V2` was not exported |
| `apfs: container 0: N blocks of 4096 do not fit the device` | `root.apfs` changed size and `payload` was not rerun |
| Touch reads all zeros, ghost address acknowledges | Touch rails are off, or SDA is held low by an unpowered controller |
| Wi-Fi endpoint id reads all ones | REG_ON timing, PERST#, or the PHY reference clock |
| Wi-Fi HVC panics | Samsung's EL2 is not present on this boot path |
| Kernel image too large | The 16 MiB slot is full; move `root.apfs` or remove a subsystem |
| Build ends with `check_all_exports` | Expected; the kernel is linked |
| `stat -f %z` fails | GNU `stat` is on `PATH`; use `stat -c %s` or `wc -c` |
| `loader_return_after` does nothing | It does not work on this phone; use the key or touch gesture |

## Recovery

- Force reset: Volume Down and Power for 10 to 15 seconds.
- Download Mode: Volume Down and Power for about 6 seconds, then Volume Up.
- Clear the boot-recovery flag: flash the stock `misc` image. Never zero the whole partition.
- Restore Android's boot image: flash `backup-y2s/boot.img` to `boot`.
- Keep the partition backups and a stock AP tar. They are the difference between a bad afternoon and a dead phone.
