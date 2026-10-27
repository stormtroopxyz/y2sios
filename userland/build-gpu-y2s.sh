#!/bin/bash
# The GPU pieces of the small image for the Galaxy S20+ (y2s, Mali-G77), built on macOS, where
# build.sh does not compile (it wants the Linux toolchain's ld64 and falls back to prebuilt/userland):
#
#   obj/libOSMesa.8.dylib      Mesa with panfrost (tools/mk-mesa.sh, DRIVERS=softpipe,panfrost)
#   obj/libredwood-gl.dylib    CoreAnimation's OpenGL ES (src/eagl.c, src/gl_osmesa.c)
#   obj/gl_test, obj/mali_test the tests (src/gl_test.c, src/mali_test.c)
#   obj/libredwoodair.dylib    Metal's shader compiler on panfrost (AIR=1 tools/mk-mesa.sh), and the
#   obj/RedwoodMetal, obj/libredwoodmetal.dylib, obj/metal_drv_test, obj/metal_props   Metal driver
#   prebuilt/userland/libredwood.dylib   src/inject.c, which loads libredwood-gl.dylib into backboardd
#                              when the kernel has a GPU service (RedwoodMali: boot-arg mali=1), and
#                              libredwood-sb.dylib (src/inject_sb.c, SpringBoard's, which asks the same);
#                              the ones that were there are kept as *.orig
#
# then build.sh with GL=1 and whatever else is given in the environment (REST=1, RC_JOBS=...).
#   userland/build-gpu-y2s.sh                  REST=1 GL=1
#   RC_JOBS=rc.jobs.mali userland/build-gpu-y2s.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
SDK=${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk
DYN="$ROOT/xnu-deps/host-macos/bin/xnu-clang -arch arm64 -isysroot $SDK -O1"
M=$ROOT/xnu-deps/build/mesa/out

if [ "$(cat $M/built-from 2>/dev/null)" != "$(cat ../tools/mesa/*.patch ../tools/mesa/redwood-drm/* | sha256sum)" ] ||
    ! grep -q panfrost $M/drivers 2>/dev/null; then
	DRIVERS=softpipe,panfrost ../tools/mk-mesa.sh
fi
# ... and the compiler library whenever anything it is built from changed (tools/mk-mesa.sh writes the same
# hash): its sources, the libdrm stand-in compiled into it, the patches. A driver built against a newer rwa_*
# contract than the library beside it reads one field as another, and a stale shim measures nothing
if [ "$(cat $M/built-from-air 2>/dev/null)" != "$(cat ../tools/mesa/air/* ../tools/mesa/redwood-drm/* ../tools/mesa/*.patch | sha256sum)" ]; then
	echo "build-gpu-y2s.sh: libredwoodair.dylib is older than what it is built from"
	AIR=1 DRIVERS=softpipe,panfrost ../tools/mk-mesa.sh
fi
cp $M/libOSMesa.8.dylib obj/
./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation -framework IOKit \
	-framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-gl.dylib src/eagl.c src/gl_osmesa.c
$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
$DYN -framework IOKit -o obj/mali_test src/mali_test.c
# Metal (PLANS/05-metal.md): the compiler library, the driver as a library and as the bundle Metal's
# discovery loads, and the programs that drive it
[ -f $M/libredwoodair.dylib ] && cp $M/libredwoodair.dylib obj/
$DYN -dynamiclib -install_name /redwood/lib/libredwoodmetal.dylib -framework Foundation -framework IOKit \
	-framework Metal -lobjc -o obj/libredwoodmetal.dylib src/redwoodmetal.m src/redwooddrawable.m
$DYN -bundle -framework Foundation -framework IOKit -framework Metal -lobjc \
	-o obj/RedwoodMetal src/redwoodmetal.m src/redwooddrawable.m
$DYN -framework Foundation -framework IOKit -framework Metal -lobjc -o obj/metal_drv_test src/metal_drv_test.m
$DYN -framework CoreFoundation -framework IOKit -o obj/metal_props src/metal_props.c
# the log daemon: build.sh's own compile line is skipped on macOS (no ld64), so it is built here
# whenever it changes -- /menu (hand back to the loader) lives in it
$DYN -o obj/rwlogd src/rwlogd.c
cp obj/rwlogd ../prebuilt/userland/rwlogd   # the macOS path copies prebuilt/* into obj/, so that is where it must land
# mali2metal's marker for scripts (src/m2m_mark.c; the tour, m2m.tour, uses it)
$DYN -o obj/m2m_mark src/m2m_mark.c
cp obj/m2m_mark ../prebuilt/userland/m2m_mark
$DYN -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc \
	-o obj/libredwood.dylib.new src/inject.c
$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
	-o obj/libredwood-sb.dylib.new src/inject_sb.c
for lib in libredwood libredwood-sb; do
	[ -f ../prebuilt/userland/$lib.dylib.orig ] || cp ../prebuilt/userland/$lib.dylib ../prebuilt/userland/$lib.dylib.orig
	mv obj/$lib.dylib.new ../prebuilt/userland/$lib.dylib
done
# process 1 (src/init.c), static against libmini as build.sh would with ld64; Apple's ld makes the same layout
STATIC="$ROOT/xnu-deps/host-macos/bin/xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin"
$STATIC -Ilibmini -c libmini/mini.c -o obj/mini.o
$STATIC -c libmini/start.S -o obj/start.o
$STATIC -Ilibmini -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o obj/init.new src/init.c obj/mini.o obj/start.o
[ -f ../prebuilt/userland/init.orig ] || cp ../prebuilt/userland/init ../prebuilt/userland/init.orig
cmp -s obj/init.new ../prebuilt/userland/init && rm obj/init.new || mv obj/init.new ../prebuilt/userland/init

PATH=$ROOT/xnu-deps/host-macos/bin:$PATH REST=${REST:-1} GL=1 EXTRA="${EXTRA:-} obj/mali_test" ./build.sh
