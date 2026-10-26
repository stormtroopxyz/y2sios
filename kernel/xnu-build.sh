#!/bin/bash
# Build XNU for redwood on Linux or macOS. Extra make args are passed through.
source "$(dirname "${BASH_SOURCE[0]}")/xnu-env.sh"
cd "$XNU_SRC" || exit 1

# a portable file mtime (GNU stat -c %Y, BSD stat -f %m)
mtime() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null; }

# The Wi-Fi driver's object (tools/mk-ath11k.sh) is linked into the kernel when it is there; make
# does not see it change, so the linked kernel is thrown away when it has.
LX=$XNUDEPS/build/ath11k/out/ath11k.o
STAMP=BUILD/obj/redwood-objects
if [ -f "$LX" ]; then now="$LX $(mtime "$LX")"; else LX= now=; fi
if [ -d BUILD/obj ] && [ "$(cat $STAMP 2>/dev/null)" != "$now" ]; then
	rm -f BUILD/obj/*/mach.*.unstripped
	echo "$now" > $STAMP
fi

# Host tools. On Linux the kernel's SETUP tools are compiled against a glibc compatibility
# header and use GNU m4. On macOS they are the native Command Line Tools (mig, gm4, ...),
# so nothing is overridden -- see common/macos-env.sh and xnu-deps/setup.sh.
HOSTENV=()
case "$(uname -s)" in
Darwin)
	HOSTENV=(HOST_FLEX=flex HOST_BISON="${HOST_BISON:-bison}" HOST_GM4=gm4)
	;;
*)
	HOSTENV=(
		HOST_CC="clang -include $XNUDEPS/hostinc/hostcompat.h -I$XNUDEPS/hostinc -Wno-unused-command-line-argument -Wno-error=implicit-function-declaration"
		HOST_FLEX=flex HOST_BISON=bison HOST_GM4=m4
	)
	;;
esac

# A GICv2 board (y2s, Exynos 990) selects osfmk/arm64/gic_v2_irq.c instead of the GICv3 one.
# Set by common/devices/y2s.conf (GIC_V2=1); see docs/GICV2.md.
[ "$GIC_V2" = 1 ] && HOSTENV+=(MACHINE_FLAGS_ARM64_BCM2837="-DARM64_BOARD_CONFIG_BCM2837 -Xclang -target-feature -Xclang +pan -DGENERIC_GIC_V2_IRQ=1")

exec make REDWOOD_OBJECTS="$LX" SDKROOT=iphoneos ARCH_CONFIGS=ARM64 MACHINE_CONFIGS=${MACHINE:-BCM2837} KERNEL_CONFIGS=DEVELOPMENT \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64 BUILD_WERROR=0 BUILD_LTO=0 DO_CTFMERGE=0 \
  CC=xnu-clang CXX=xnu-clang++ LOGCOLORS=n "${HOSTENV[@]}" "$@"
