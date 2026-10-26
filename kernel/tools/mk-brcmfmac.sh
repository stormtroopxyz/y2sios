#!/bin/bash
# Build the driver of the y2s's Wi-Fi radio for our kernel: brcmfmac, Broadcom's FullMAC driver
# as it is in Linux, for the BCM4375 B1 (PCI id 14e4:4475) on the Exynos 990's PCIe channel 0,
# on the same layer that tools/mk-ath11k.sh builds Qualcomm's ath11k on. Three things meet here,
# and only the third is in the repository:
#
#   Linux's brcmfmac      drivers/net/wireless/broadcom/brcm80211/{brcmfmac,brcmutil,include} and
#                         include/linux/platform_data/brcmfmac.h of a pinned release of Linux
#                         (ISC, every file: the licence is checked, not assumed); patched with
#                         wifi/lx/patches/brcmfmac/*.patch for what z3s found the BCM4375 of this
#                         SoC needs (its ids, RAM base and firmware name, legacy INTx, host_cap);
#   FreeBSD's LinuxKPI    the headers with which FreeBSD compiles Linux drivers, as for ath11k
#                         (the same release, the same wifi/lx/patches/*.patch);
#   wifi/lx               ours: wifi/lx/include and wifi/lx/src/lx_base.c, shared with ath11k
#                         (locks, sleeping, work, timers, memory the radio reads and writes, socket
#                         buffers, firmware files), and what brcmfmac alone needs:
#                         wifi/lx/include/brcmfmac (the few Linux headers outside the driver that it
#                         includes, written anew) and wifi/lx/src/brcmfmac (the PCI device, its
#                         interrupt, the network device and cfg80211 underneath the driver, and its
#                         entry points). wifi/lx/src/brcmfmac/STUBS.md says what is real and what not.
#
# Only the PCIe FullMAC path is built: DRIVER below. SDIO, USB, BCDC/fwsignal, debugfs, tracing,
# OF/DMI/ACPI are not; p2p, pno, btcoex and vendor commands are not either. What the built files
# call in those becomes a function that says its name in the log and answers 0 (out/stubs.txt),
# unless wifi/lx/src has one that does more.
#
# Everything is compiled as FreeBSD kernel code for an Apple arm64 kernel and tied into one object
# that shows nothing but its entry points (lx_prepare, lx_start, lx_stop, lx_transmit of the
# kernel's bsd/sys/redwood_lx.h). It is NOT linked into the kernel yet (xnu-build.sh links
# ath11k's object only): what it asks of the kernel beyond redwood_lx.h is declared in
# wifi/lx/include/brcmfmac/lx_os_pci.h, and the kernel has none of it so far.
#
#   VERSION   of Linux, and FREEBSD: those of tools/mk-ath11k.sh, whose downloads are used when
#             they are there (xnu-deps/build/ath11k), else fetched into xnu-deps/build/brcmfmac.
#
# What comes out, in xnu-deps/build/brcmfmac:
#   src/               the driver's files, patched           freebsd/sys/   FreeBSD's, patched
#   out/brcmfmac.o     the object for the kernel             out/stubs.txt  what is not there yet
# Needs: curl, tar, xz, sha256sum, patch, awk, clang, and a Mach-O linker (xnu-deps/host/bin/ld64
# on Linux, see xnu-deps/setup.sh; the system's ld on macOS), llvm-nm and llvm-size (or xcrun's).
#
# usage: tools/mk-brcmfmac.sh [fetch]      "fetch" stops when the driver's sources are there
set -e
VERSION=7.0.14
SHA256=de9999b784d2293f00d39c62d8f92a08ab8a54bc4e80ffd250a0c09cb07a0f98
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
B=$ROOT/xnu-deps/build/brcmfmac
A=$ROOT/xnu-deps/build/ath11k			# tools/mk-ath11k.sh's downloads, if it has made them
BRCM=drivers/net/wireless/broadcom/brcm80211
W=$ROOT/wifi/lx
# the kernel's tree (redwood_lx.h); on macOS also the GNU tools, which the rest is written for
source "$ROOT/xnu-env.sh"
mkdir -p "$B"
cd "$B"

fetch() {			# fetch FILE URL: ath11k's copy if there is one, else the network's
	[ -f "$1" ] && return
	if [ -f "$A/$1" ]; then
		ln -s "$A/$1" "$1"
	else
		curl -sL --fail -o "$1.part" "$2" && mv "$1.part" "$1"
	fi
}

# --- the driver's sources: unpacked, every file's licence checked, then our patches. When the
# patches change, the sources are taken from the archive again.
LPATCHES=$(cat "$W"/patches/brcmfmac/*.patch 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat src/.release 2>/dev/null)" != "$VERSION $SHA256 $LPATCHES" ]; then
	fetch linux-$VERSION.tar.xz https://cdn.kernel.org/pub/linux/kernel/v${VERSION%%.*}.x/linux-$VERSION.tar.xz
	echo "$SHA256  linux-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf src unpacked obj
	mkdir unpacked
	tar xJf linux-$VERSION.tar.xz -C unpacked --strip-components=1 linux-$VERSION/$BRCM/brcmfmac \
		linux-$VERSION/$BRCM/brcmutil linux-$VERSION/$BRCM/include \
		linux-$VERSION/include/linux/platform_data/brcmfmac.h
	# every file must say itself that it may be used so: ISC, by its SPDX tag or spelt out
	for f in $(find unpacked -name '*.[ch]'); do
		case "$(head -6 "$f")" in
		*"SPDX-License-Identifier: ISC"*) ;;
		*"Permission to use, copy, modify, and/or distribute this software"*) ;;
		*) echo "mk-brcmfmac.sh: $f does not carry the licence expected" >&2; exit 1 ;;
		esac
	done
	mkdir -p src/linux/platform_data
	mv unpacked/$BRCM/brcmfmac unpacked/$BRCM/brcmutil unpacked/$BRCM/include src/
	mv unpacked/include/linux/platform_data/brcmfmac.h src/linux/platform_data/
	rm -rf unpacked
	for p in "$W"/patches/brcmfmac/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d src -p1 < "$p"
	done
	echo "$VERSION $SHA256 $LPATCHES" > src/.release
fi
[ "$1" = fetch ] && { echo "mk-brcmfmac.sh: brcmfmac of Linux $VERSION: $B/src/brcmfmac"; exit 0; }

# --- FreeBSD's LinuxKPI and the kernel headers under it, with the patches ath11k is built with
FREEBSD=15.1
FREEBSD_SHA256=cf5762da53fd52e1eaf0f9ceee9bf58cbe314c821031d0d9ffa76823185a89e1
FSYS="compat/linuxkpi sys arm64/include net net80211 netinet netinet6 vm kern tools libkern ddb
	security fs/pseudofs crypto opencrypto geom bsm isa cam rpc xdr x86/include ofed/include
	dev/pci dev/ofw dev/fdt dev/acpica dev/usb dev/iicbus dev/backlight
	contrib/ck/include contrib/dev/acpica/include contrib/libnv"
PATCHES=$(cat "$W"/patches/*.patch 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ "$(cat freebsd/.release 2>/dev/null)" != "$FREEBSD $FREEBSD_SHA256 $PATCHES" ]; then
	fetch freebsd-$FREEBSD-src.txz https://download.freebsd.org/releases/amd64/$FREEBSD-RELEASE/src.txz
	echo "$FREEBSD_SHA256  freebsd-$FREEBSD-src.txz" | sha256sum -c --quiet
	rm -rf freebsd gen obj
	mkdir freebsd
	tar xJf freebsd-$FREEBSD-src.txz -C freebsd --strip-components=2 usr/src/COPYRIGHT \
		$(for d in $FSYS; do echo usr/src/sys/$d; done)
	for p in "$W"/patches/*.patch; do
		[ -f "$p" ] && patch -s --no-backup-if-mismatch -d freebsd -p1 < "$p"
	done
	echo "$FREEBSD $FREEBSD_SHA256 $PATCHES" > freebsd/.release
fi
S=$B/freebsd/sys

# --- what FreeBSD's build would have generated: the interfaces of its device framework, and the
# option headers, which here say only that there is an Internet
if [ ! -d gen ]; then
	mkdir gen
	(
		cd gen
		ln -s $S/arm64/include machine
		for m in kern/device_if.m kern/bus_if.m dev/pci/pci_if.m dev/pci/pci_iov_if.m dev/pci/pcib_if.m \
			dev/iicbus/iicbus_if.m dev/iicbus/iicbb_if.m dev/backlight/backlight_if.m dev/usb/usb_if.m \
			dev/ofw/ofw_bus_if.m; do
			awk -f $S/tools/makeobjops.awk $S/$m -h
		done
		for o in h p q; do awk -f $S/tools/vnode_if.awk $S/kern/vnode_if.src -$o; done
		for o in $(grep -rhoE "opt_[a-z0-9_]+\.h" $S | sort -u) lkpi_iic_if.h; do : > $o; done
		printf '#define INET 1\n#define INET6 1\n' > opt_inet.h
		printf '#define INET6 1\n' > opt_inet6.h
		printf '#define SMP 1\n' > opt_global.h
	)
fi

# --- compile: the driver's files that are built, and ours. Linux's configuration is a built-in
# brcmfmac (no vendor modules to load: wcc/core.c is in) with the PCIe bus and the msgbuf protocol.
# DEBUG makes brcmf_dbg() real; which of its kinds are said is the kernel's option wifi_brcmf_debug.
DRIVER="brcmfmac/pcie brcmfmac/msgbuf brcmfmac/commonring brcmfmac/flowring brcmfmac/proto brcmfmac/core
	brcmfmac/fwil brcmfmac/fweh brcmfmac/firmware brcmfmac/chip brcmfmac/feature brcmfmac/cfg80211
	brcmfmac/common brcmfmac/xtlv brcmfmac/fwvid brcmfmac/wcc/core brcmutil/utils brcmutil/d11"
GLUE="$W/src/lx_base.c $(ls "$W"/src/brcmfmac/*.c)"
CC="clang -target arm64-apple-ios16.5"
case "$(uname -s)" in
Darwin)
	LD=$ROOT/xnu-deps/host/bin/ld64
	[ -x "$LD" ] || LD=$(command -v ld)
	NM=$(command -v llvm-nm || xcrun -f llvm-nm)
	SIZE=$(command -v llvm-size || xcrun -f llvm-size)
	;;
*)
	LD=$ROOT/xnu-deps/host/bin/ld64 NM=llvm-nm SIZE=llvm-size
	;;
esac
CFLAGS="-mkernel -mgeneral-regs-only -fno-builtin -fno-common -ffreestanding -fno-strict-aliasing
	-funsigned-char -nostdinc -std=gnu17 -O2 -g -w
	-D__FreeBSD__=15 -D_KERNEL -DKLD_MODULE -DHAVE_KERNEL_OPTION_HEADERS -include opt_global.h
	-I$W/include/brcmfmac -I$W/include -Igen -I$S -I$S/contrib/ck/include
	-I$S/compat/linuxkpi/common/include -I$S/compat/linuxkpi/dummy/include
	-include $S/compat/linuxkpi/common/include/linux/kconfig.h -DLINUXKPI_VERSION=70000"
DFLAGS="-Isrc/brcmfmac -Isrc/include -Isrc -include $W/include/brcmfmac/lx_brcmfmac.h
	-DCONFIG_BRCMFMAC=1 -DCONFIG_BRCMFMAC_PCIE=1 -DCONFIG_BRCMFMAC_PROTO_MSGBUF=1 -DDEBUG=1
	-DKBUILD_MODNAME=\"brcmfmac\""
# the kernel's description of what it offers this object (wifi/lx/include/lx_os.h)
[ -f "$XNU_SRC/bsd/sys/redwood_lx.h" ] || { echo "mk-brcmfmac.sh: the kernel tree has no bsd/sys/redwood_lx.h" >&2; exit 1; }
DFLAGS="$DFLAGS -DLX_REDWOOD_LX_H=\"$XNU_SRC/bsd/sys/redwood_lx.h\""
mkdir -p obj out
rm -f obj/*.err
compile() {		# compile SOURCE OBJECT [FLAGS]: only when the object is older than anything it was made of
	local src=$1 obj=$2
	shift 2
	if [ -f "$obj" ] && [ -f "$obj.d" ] && [ -z "$(find $(sed -e 's/^[^:]*://' -e 's/\\$//' "$obj.d") "$0" -newer "$obj" 2>/dev/null | head -1)" ]; then
		return
	fi
	$CC $CFLAGS "$@" -MMD -MF "$obj.d" -c "$src" -o "$obj" 2> "$obj.err" || rm -f "$obj"
}
name() { echo "$1" | tr / _; }
for f in $DRIVER; do compile src/$f.c obj/brcmf_$(name $f).o $DFLAGS & done
for f in $GLUE; do compile "$f" obj/lx_$(basename "$f" .c).o $DFLAGS -I$W/src -I$W/src/brcmfmac & done
wait
bad=
for f in obj/*.err; do
	[ -s "$f" ] && [ ! -f "${f%.err}" ] && { echo "--- $(basename "$f" .o.err)"; grep -m6 -A3 "error:" "$f"; bad=1; }
done
[ -z "$bad" ] || exit 1
OBJECTS=$(for f in $DRIVER; do echo obj/brcmf_$(name $f).o; done; for f in $GLUE; do echo obj/lx_$(basename "$f" .c).o; done)

# --- tie it all into one object. What it then still wants is one of three things: a call of the
# kernel's (redwood_lx.h, lx_os_pci.h; the handful of C library functions XNU has under the same
# names with the same meaning), a function of a part of the driver that is not built (p2p.c,
# pno.c, btcoex.c, vendor.c, debug.c, ...), or a mistake. The second kind gets a stub each that
# says its name and answers 0; anything else missing ends the build.
KERNEL="memcmp memcpy memmove memset strcmp strncmp strlen strchr strrchr strlcpy strlcat bzero
	snprintf vsnprintf __stack_chk_fail __stack_chk_guard"
$LD -r -arch arm64 $OBJECTS -o out/all.o 2> out/ld.err || { cat out/ld.err; exit 1; }
$NM -u --format=just-symbols out/all.o | sed 's/^_//' | sort > out/undefined.txt
: > out/stubs.txt
: > out/stubs.s
missing=
for s in $(cat out/undefined.txt); do
	case " $(echo $KERNEL) " in *" $s "*) continue ;; esac
	case $s in
	lx_os_*|section\$*) continue ;;
	brcmf_*|__brcmf_*) ;;
	*) echo "mk-brcmfmac.sh: nothing defines $s" >&2; missing=1; continue ;;
	esac
	echo $s >> out/stubs.txt
	printf '\t.globl _%s\n_%s:\n\tadrp x0, 1f@PAGE\n\tadd x0, x0, 1f@PAGEOFF\n\tb _lx_stub\n\t.cstring\n1:\t.asciz "%s"\n\t.text\n\t.p2align 2\n' \
		$s $s $s >> out/stubs.s
done
[ -z "$missing" ] || exit 1
$CC -c out/stubs.s -o out/stubs.o
# four names are shown: where the kernel enters (lx_prepare(), lx_start(), lx_stop() and lx_transmit() of redwood_lx.h)
printf '_lx_prepare\n_lx_start\n_lx_stop\n_lx_transmit\n' > out/exported.txt
$LD -r -arch arm64 -exported_symbols_list out/exported.txt out/all.o out/stubs.o -o out/brcmfmac.o 2> out/ld.err ||
	{ cat out/ld.err; exit 1; }
rm -f out/all.o
$SIZE -m out/brcmfmac.o > out/size.txt 2>/dev/null || true
echo "mk-brcmfmac.sh: out/brcmfmac.o: $($SIZE out/brcmfmac.o | awk 'NR == 2 { print $1 + $2 }') bytes of code and data" \
	"($($SIZE out/brcmfmac.o | awk 'NR == 2 { print $1 }') of __TEXT) from $(echo $DRIVER | wc -w) of the driver's files;" \
	"$(wc -l < out/stubs.txt) functions of the others are stubs (out/stubs.txt);" \
	"asks the kernel for $($NM -u --format=just-symbols out/brcmfmac.o | grep -vc 'section\$') names"
