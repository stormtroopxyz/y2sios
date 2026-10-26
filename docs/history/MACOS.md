# Building HackiOS natively on macOS (Apple silicon)

Yes — you do **not** need a Linux machine. macOS is the *native* home of both pieces:
theos runs on macOS, and XNU is Apple's own source. On an M‑series Mac you build
**arm64 → arm64**; on the documented x86‑64 Linux host it is a cross build.

**Verified working:** on Apple silicon, `xnu/apply.sh` → `xnu-deps/setup.sh` →
`./xnu-build.sh -j10 -k` produces a valid `Mach-O 64-bit executable arm64`
(`mach.development.bcm2837`, ~16 MB) with **0 compile errors**. The command still
exits **non‑zero on purpose** — the only failures left are XNU's known post‑link
step, the `config/*.symbolset` export check (see `xnu/README.md`); the kernel is
linked and usable by then.

This document is for the **kernel** half (`hackios-xnu-kernel`). The userland
(`hackios-xnu-userland`) is theos‑based and native. The Mu‑Silicium UEFI is a later
milestone (M1).

## 1. Why it works with so little

The XNU build invokes a set of **host tools**: `mig`, `migcom`, `gm4`, `ctf_insert`,
`unifdef`, `lipo`, `nm`, `otool`, `strip`, `libtool`, `nmedit`,
`codesign_allocate`, `install_name_tool`. On Linux the project *builds* these (a
glibc port of `mig`, plus `tcsh`/`unifdef`) because Linux has none of them. On macOS
the **Command Line Tools already provide all of them** — so full Xcode is not
required for the host tools. Only the **iPhoneOS SDK** is missing, and theos
provides that.

## 2. One-time setup

```sh
# 1. Xcode Command Line Tools (or full Xcode)
xcode-select --install

# 2. theos + the iPhoneOS 16.5 SDK (only the SDK is needed for the kernel)
mkdir -p ~/theos
git clone --depth 1 --filter=blob:none --sparse https://github.com/theos/sdks ~/theos/sdks
git -C ~/theos/sdks sparse-checkout set iPhoneOS16.5.sdk     # -> ~/theos/sdks/iPhoneOS16.5.sdk

# 3. GNU userland + the rest
brew install coreutils gnu-tar gnu-sed findutils bison qemu sevenzip lz4 dtc

# 4. let the build see the GNU tools (works in bash and zsh)
echo 'source /path/to/hackios-xnu-kernel/common/macos-env.sh' >> ~/.zshrc
```

`common/macos-env.sh` puts Homebrew's `coreutils`/`gnu-tar`/`gnu-sed`/`findutils`
and this project's macOS shims on `PATH`. It is also sourced automatically by
`xnu-env.sh` and `common/lib.sh`, so `common/build.sh` works even without the
`.zshrc` line — but `xl`, `qemu/*.sh`, `userland/build.sh` and `tools/*` use
`readlink -f`/`stat -c` directly, so the profile line is the general fix.

## 3. Build the kernel

```sh
cd /path/to/hackios-xnu-kernel
xnu/apply.sh                 # Apple's XNU xnu-8796.121.2 + redwood.patch + files/
xnu-deps/setup.sh            # stage 1 (macOS branch) + stage 2
./xnu-build.sh -j"$(nproc)" -k
```

Result: `xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.

## 4. What was changed for macOS (Linux path untouched; guarded by `uname -s`)

| File | Change |
|---|---|
| `common/macos-env.sh` **(new)** | Homebrew GNU tools + the project's macOS shims onto `PATH`; works when sourced from bash **or** zsh. |
| `xnu-deps/host-macos/bin/xcrun` **(new)** | Answers target‑SDK queries with the overlay (`xnu-deps/sdk/iPhoneOS16.5.xnu.sdk`); delegates `-find`/host queries to the real `/usr/bin/xcrun`; resolves tools the CLT keep out of `PATH` (**`migcom`** lives in `usr/libexec`, next to `mig`). |
| `xnu-deps/host-macos/bin/python` **(new)** | `python` → `python3` (XNU's dSYM macro step calls `env python`). |
| `xnu-deps/host/bin/xnu-clang` | Host‑aware: Apple clang + system linker on Darwin; cctools `ld64` on Linux. |
| `xnu-env.sh` | Sources `macos-env.sh` on Darwin instead of putting the **Linux** shims (`xnu-deps/host/bin`) on `PATH`. |
| `xnu-build.sh` | On Darwin: no glibc `hostcompat.h`/`HOST_CC`, `gm4`, and a portable `mtime()`. |
| `xnu-deps/setup.sh` | Adds `stage1_macos()` (no mig/tcsh/unifdef to build; links `iig`/`xnu-clang`/`install`; builds the SDK overlay; extracts the DriverKit headers) and a host‑aware `stage2` guard. |
| `xnu-deps/hostinc/xnu_clang_compat.h` | Defines **`XNU_NO_TYPE_BUILTINS 0`** when the compiler *has* Apple's type builtins (Apple clang does; Linux clang does not). Without this, `OSData.h`/`kalloc.h` fail on macOS. |
| `xnu-deps/host/bin/install` (used on macOS via a link) | BSD‑style `install`: Homebrew coreutils' **GNU `install`** reads XNU's `-S` flag as "backup suffix" and breaks the `version.h` rule. |
| `common/lib.sh`, `common/{build,run,new-device,setup}.sh` | Portable top‑of‑script `readlink -f` replacement. |
| `xnu/redwood.patch` (`MakeInc.top`) | `MEMORY_SIZE` falls back to `/usr/sbin/sysctl -n hw.memsize` when `/proc/meminfo` is absent. |

## 5. Quick sanity checks

```sh
source common/macos-env.sh
xcrun -sdk iphoneos -show-sdk-path      # -> …/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk
xcrun -sdk macosx  -find migcom         # -> …/CommandLineTools/usr/libexec/migcom
xcrun -sdk macosx  -find iig            # -> …/xnu-deps/host-macos/bin/iig
```

## 6. Notes / residual

- The build **ends non‑zero on purpose**: `config/Libkern.symbolset` and
  `config/IOKit.symbolset` (the export check) fail after the kernel is linked. That
  is documented in `xnu/README.md` and is not a macOS problem.
- The local kernel differs slightly from `prebuilt/` (built with a different clang),
  which is expected; it is a valid arm64 Mach‑O regardless.
- `bison`: the system one is 2.3; `macos-env.sh` exports `HOST_BISON` from Homebrew.
- **Mu‑Silicium UEFI** (M1) is *not* covered here; EDK2/Project Mu on macOS is a
  separate exercise.

## 7. Test loop unchanged

Once the kernel builds, QEMU is the fast loop (no phone): `common/build.sh qemu-virt`
then `common/run.sh qemu-virt`. For the GICv2 work, see `docs/GICV2.md`.
