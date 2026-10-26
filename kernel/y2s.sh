#!/bin/bash
# y2s.sh: build, pack, flash, and read back the kernel log for the Galaxy S20+ (y2s).
#
#   ./y2s.sh kernel                 build XNU (GIC_V2 exported), copy to out/mach.y2s, export xnu/
#   ./y2s.sh payload "<cmdline>"    rebuild the XNUP payload with this cmdline, splice it into out/y2s-embed.img
#   ./y2s.sh cmdline                print the cmdline currently in out/y2s-embed.img
#   ./y2s.sh pack                   out/y2s-recovery.img = embed@0 + kernel@32 MiB + root.apfs@48 MiB
#   ./y2s.sh flash                  adb: write out/y2s-recovery.img to recovery, verify, reboot into it
#   ./y2s.sh log                    adb: pull the 8 MiB log at recovery+16 MiB, print it (also ../xnulog.txt)
#   ./y2s.sh mlog                   the last run's Metal and display lines only (USB if the phone is
#                                   in Log access, else Wi-Fi), so a log can be read for those alone
#   ./y2s.sh ship "<cmdline>"       payload + pack + flash
#
# Without Android (PLANS/09-standalone.md): the loader's menu -> "Log access (USB)", then
#   ./y2s.sh ulog                   the last run's log over USB (also ../xnulog.txt)
#   ./y2s.sh uflash                 out/y2s-recovery.img to recovery over USB, read back by the phone
#                                   (and the boot image to `boot` too once `standalone` was done)
#   ./y2s.sh uboot                  leave Log access and boot iOS
#   ./y2s.sh hflash                 from Samsung's Download Mode: heimdall writes recovery (and boot)
#
# While iOS runs, over Wi-Fi (rwlogd on the phone, port 8080, Bonjour "HackiOS"; or Y2S_IP=address):
#   ./y2s.sh wlog                   the kernel's log so far (also ../xnulog.txt)
#   ./y2s.sh wtail                  follow it live (Ctrl-C ends)
#   ./y2s.sh hlogo                  (Download Mode) the boot splash: the Apple logo instead of Samsung's
#   ./y2s.sh standalone             (once, asks first) the boot image into `boot`: power-on goes to the
#                                   menu, Android no longer boots (back: heimdall --BOOT ../backup-y2s/boot.img)
#
# Without a cmdline, payload and ship use DEFAULT_CMDLINE: the configuration measured best so far
# (PLANS/01-performance.md). Never give cpus= and cpumask= together: XNU panics.
#
# Partition layout: PLANS/00-foundations.md, handoff.md section 4.
set -euo pipefail
cd "$(dirname "$0")"

EMBED=out/y2s-embed.img
PAYLOAD=out/y2s-payload.bin
PAYLOAD_AT=$((0x100870))                # FD at page + 112 = 0x870, payload at FD + 0x100000
PAYLOAD_MAX=$((0x80000))
DEFAULT_CMDLINE="touch_ts=1 bms=1 mali=1 cpumask=0x3f fb_threads=6"   # 4x A55 + 2x A76; the M5s idle at 266 MHz (no DVFS yet)
KERNEL_BUILD=xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837

die() { echo "y2s.sh: $*" >&2; exit 1; }

cmd_kernel() {
	set +u
	. common/macos-env.sh
	set -u
	export GIC_V2=1
	touch out/.kernel-start
	./xnu-build.sh -j"$(sysctl -n hw.ncpu)" -k > out/kernel-build.log 2>&1 || true
	if grep -E '^[^ ]*\.(c|cpp|s|h):[0-9]+:[0-9]+: (fatal error|error|warning):' out/kernel-build.log; then
		die "compiler errors/warnings above (full log: out/kernel-build.log)"
	fi
	[ "$KERNEL_BUILD" -nt out/.kernel-start ] || echo "kernel: nothing changed, not relinked"
	strings "$KERNEL_BUILD" | grep 'gic: v2' > /dev/null || die "kernel is not GICv2 (GIC_V2 not exported?)"
	cp "$KERNEL_BUILD" out/mach.y2s
	[ "$(wc -c < out/mach.y2s)" -le $((0x1000000)) ] || die "kernel larger than its 16 MiB slot"
	xnu/export.sh
	echo "kernel: out/mach.y2s ($(wc -c < out/mach.y2s) bytes)"
}

cmd_payload() {
	local cmdline=${1:-$DEFAULT_CMDLINE}
	case " $cmdline " in
	*" cpus="*" cpumask="* | *" cpumask="*" cpus="*) die "cpus= and cpumask= together make XNU panic" ;;
	esac
	local rsize
	# the loader for this phone: GIC_V2 (the GICv3 system registers are undefined here: a loader built
	# without it takes a UEFI exception reading ICC_PMR_EL1 on the way into the kernel)
	GIC_V2=1 xnuloader/build.sh > /dev/null
	rsize=$(printf '%x' "$(wc -c < userland/root.apfs)")     # (root.apfs grows with GL=test: libOSMesa)
	python3 xnuloader/mkpayload.py -o "$PAYLOAD" loader.efi=xnuloader/loader.efi \
		--kpart recovery --koff 2000000 --rpart recovery --roff 3000000 --rsize "$rsize" \
		--rdpart super --rdoff 184b00000 --rdsize 948c0000 ${cmdline:+--cmdline "$cmdline"}
	python3 - "$EMBED" "$PAYLOAD" "$PAYLOAD_AT" "$PAYLOAD_MAX" <<'PY'
import sys
embed, payload, at, limit = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
img = bytearray(open(embed, 'rb').read())
p = open(payload, 'rb').read()
assert img[at:at + 4] == b'XNUP', 'no XNUP payload at the expected offset of %s' % embed
assert len(p) <= limit, 'payload too large'
img[at:at + limit] = b'\xff' * limit      # the FD's erased fill
img[at:at + len(p)] = p
open(embed, 'wb').write(img)
PY
	echo "payload: cmdline '$cmdline' spliced into $EMBED"
}

cmd_cmdline() {
	python3 - "$EMBED" "$PAYLOAD_AT" <<'PY'
import struct, sys
d = open(sys.argv[1], 'rb').read(); i = int(sys.argv[2])
assert d[i:i + 4] == b'XNUP'
i += 8
while True:
    name = d[i:i + 16].rstrip(b'\0'); size = struct.unpack('<I', d[i + 16:i + 20])[0]
    if not name:
        break
    if name == b'cmdline':
        print(d[i + 24:i + 24 + size].rstrip(b'\0').decode())
    i += 24 + (size + 15) // 16 * 16
PY
}

cmd_pack() {
	python3 - "$EMBED" "$PAYLOAD_AT" <<'PY'
import struct, sys
ker = open('out/mach.y2s', 'rb').read()
rd = open('userland/root.apfs', 'rb').read()
img = open('out/y2s-embed.img', 'rb').read()
assert len(img) <= 0x1000000 and len(ker) <= 0x1000000
assert 0x3000000 + len(rd) <= 69009408, 'root.apfs does not fit in recovery (65.8 MiB)'
# The loader reads exactly the payload's rsize bytes of root.apfs: a rebuilt image of another size
# packed without `payload` boots to "apfs: container 0: N blocks of 4096 do not fit the device"
# and halts on the Silicium screen (25 October).
i = int(sys.argv[2]) + 8
rsize = None
while True:
    name = img[i:i + 16].rstrip(b'\0'); size = struct.unpack('<I', img[i + 16:i + 20])[0]
    if not name:
        break
    if name == b'rsize':
        rsize = int(img[i + 24:i + 24 + size].rstrip(b'\0'), 16)
    i += 24 + (size + 15) // 16 * 16
if rsize != len(rd):
    sys.exit('y2s.sh: pack: the payload says root.apfs is %s bytes and it is %d: run '
             '`./y2s.sh payload "<cmdline>"` again first' % (rsize, len(rd)))
out = bytearray(max(0x2000000 + len(ker), 0x3000000 + len(rd)))
out[0:len(img)] = img
out[0x2000000:0x2000000 + len(ker)] = ker
out[0x3000000:0x3000000 + len(rd)] = rd
open('out/y2s-recovery.img', 'wb').write(out)
print('pack: out/y2s-recovery.img (%d bytes)' % len(out))
PY
}

adb_root() {
	adb wait-for-device
	adb root > /dev/null
	sleep 2
	adb wait-for-device
}

cmd_flash() {
	adb_root
	adb push out/y2s-recovery.img /data/local/tmp/r.img
	adb shell 'dd if=/data/local/tmp/r.img of=/dev/block/by-name/recovery bs=1M 2>/dev/null && sync'
	local size there here
	size=$(wc -c < out/y2s-recovery.img)
	there=$(adb shell "head -c $size /dev/block/by-name/recovery | md5sum" | cut -d' ' -f1)
	here=$(md5 -q out/y2s-recovery.img)
	[ "$there" = "$here" ] || die "verify failed: phone $there, image $here"
	echo "flash: verified ($here)"
	adb reboot recovery
}

cmd_log() {
	adb_root
	adb shell 'dd if=/dev/block/by-name/recovery of=/data/local/tmp/l.bin bs=1M skip=16 count=8 2>/dev/null'
	adb pull /data/local/tmp/l.bin ../xnulog.bin > /dev/null
	python3 -c "
d = open('../xnulog.bin', 'rb').read().replace(b'\0', b'').decode('utf-8', 'replace')   # (a NUL inside the log once hid its second half)
open('../xnulog.txt', 'w').write(d)
print(d)"
}

FASTBOOT=${FASTBOOT:-fastboot}
HEIMDALL=../heimdall-arm64/heimdall-frontend.app/Contents/MacOS/heimdall
fb() {
	"$FASTBOOT" "$@" 2>&1 | grep -v "^Finished\.\|^Sending\|^Total time" || true
}
fb_wait() {
	local i
	for i in $(seq 1 60); do
		"$FASTBOOT" devices 2>/dev/null | grep -q fastboot && return 0
		sleep 1
	done
	die "no fastboot device: on the phone choose \"Log access (USB)\" in the menu and plug the cable in"
}
cmd_ulog() {
	fb_wait
	"$FASTBOOT" oem lastlog > /dev/null 2>&1 || die "oem lastlog failed"
	"$FASTBOOT" get_staged ../xnulog.bin > /dev/null 2>&1 || die "get_staged failed"
	python3 -c "
d = open('../xnulog.bin', 'rb').read().replace(b'\\0', b'').decode('utf-8', 'replace')
open('../xnulog.txt', 'w').write(d)
print(d)"
}
cmd_uflash() {
	local out
	fb_wait
	# in two pieces around the log (16..32 MB), so that flashing keeps the last run's log
	python3 -c "
d = open('out/y2s-recovery.img', 'rb').read()
open('out/.uflash-head', 'wb').write(d[:0x1000000])
open('out/.uflash-tail', 'wb').write(d[0x2000000:])"
	"$FASTBOOT" stage out/.uflash-head > /dev/null 2>&1 || die "stage failed"
	out=$("$FASTBOOT" oem write recovery 0 2>&1)
	echo "$out" | grep -q "written and read back" || die "recovery (boot image): $out"
	"$FASTBOOT" stage out/.uflash-tail > /dev/null 2>&1 || die "stage failed"
	out=$("$FASTBOOT" oem write recovery 2000000 2>&1)
	echo "$out" | grep -q "written and read back" || die "recovery (kernel, root): $out"
	rm -f out/.uflash-head out/.uflash-tail
	echo "uflash: recovery written and read back by the phone, the last log kept ($(md5 -q out/y2s-recovery.img))"
	if [ -f out/.standalone ]; then
		"$FASTBOOT" stage out/y2s-embed.img > /dev/null 2>&1 || die "stage failed"
		out=$("$FASTBOOT" oem write boot 0 2>&1)
		echo "$out" | grep -q "written and read back" || die "boot: $out"
		echo "uflash: boot written and read back"
		echo "uflash: the kernel and root are used from the next boot; the loader and its command line only after Reboot in the menu"
	fi
}
# The Metal and display lines of a log, and nothing else: rw_print's "redwoodmetal[...]" lines, the
# kernel's "mali:"/"fb:"/"dvfs:" lines, the display driver's own, and the lines this log exists for
# (slow commits, the 5 s summary, pipeline creation, pass shapes, traces).
metal_lines() {
	grep -aE 'redwoodmetal\[|(^|[0-9]\]: )(mali|fb|fb_scaler|dvfs):|MiniIOMobileFramebuffer|(^|[0-9]\]: )(slow|perf|trace|pass shape|pipeline) |slow: |perf: |pipeline [0-9]+:|pass shape:|trace: '
}
cmd_mlog() {
	if "$FASTBOOT" devices 2>/dev/null | grep -q fastboot; then
		cmd_ulog > /dev/null || die "oem lastlog failed"
	else
		cmd_wlog > /dev/null || die "no log: put the phone in Log access, or have iOS up on Wi-Fi (Y2S_IP=...)"
	fi
	metal_lines < ../xnulog.txt
}

cmd_uboot() {
	fb_wait
	"$FASTBOOT" oem boot > /dev/null 2>&1 || true
	echo "uboot: iOS is starting"
}
cmd_hflash() {
	if [ -f out/.standalone ]; then
		"$HEIMDALL" flash --RECOVERY out/y2s-recovery.img --BOOT out/y2s-embed.img --stdout-errors
	else
		"$HEIMDALL" flash --RECOVERY out/y2s-recovery.img --stdout-errors
	fi
}
# the boot splash Samsung's bootloader shows (up_param's logo.jpg) as the white Apple logo on black
# (out/up_param-apple.img: the backup's tar with only logo.jpg replaced). Undo: --UP_PARAM ../backup-y2s/up_param.img
# a cold restart from Log access: the new loader and its command line (uflash), then the menu boots iOS by itself
cmd_ureboot() {
	fb_wait
	"$FASTBOOT" reboot > /dev/null 2>&1
	echo "ureboot: the phone restarts; the menu boots iOS after 3 s"
}
cmd_hlogo() {
	[ -f out/up_param-apple.img ] || die "no out/up_param-apple.img"
	"$HEIMDALL" flash --UP_PARAM out/up_param-apple.img --stdout-errors
}
cmd_standalone() {
	echo "This writes the HackiOS boot image into the phone's \`boot\` partition: every power-on goes to"
	echo "the HackiOS menu and Android no longer starts (its data is untouched)."
	echo "Undo: Download Mode, then  $HEIMDALL flash --BOOT ../backup-y2s/boot.img"
	printf "Type yes to go on: "
	read -r answer
	[ "$answer" = yes ] || die "nothing written"
	fb_wait
	"$FASTBOOT" stage out/y2s-embed.img > /dev/null 2>&1 || die "stage failed"
	out=$("$FASTBOOT" oem write boot 0 2>&1)
	echo "$out" | grep -q "written and read back" || die "boot: $out"
	touch out/.standalone
	echo "standalone: boot written and read back; power-on now opens the menu"
}
phone_host() {
	local host
	if [ -n "${Y2S_IP:-}" ]; then
		echo "$Y2S_IP"
		return
	fi
	# Bonjour: "HackiOS._http._tcp" -> "... can be reached at NAME.local.:8080"
	host=$( (dns-sd -L HackiOS _http._tcp local & sleep 4; kill $! 2>/dev/null) 2>/dev/null |
		sed -n 's/.*can be reached at \([^:]*\)\.:8080.*/\1/p' | head -1)
	[ -n "$host" ] || die "the phone is not found over Bonjour: is iOS up and on Wi-Fi? (or set Y2S_IP)"
	echo "$host"
}
cmd_wlog() {
	local host
	host=$(phone_host)
	curl -sf --max-time 60 "http://$host:8080/log" -o ../xnulog.txt || die "no answer from http://$host:8080"
	cat ../xnulog.txt
}
cmd_wtail() {
	local host lt=0 dt=0 nl nd first=1
	host=$(phone_host)
	echo "== following http://$host:8080 (Ctrl-C ends) =="
	while :; do
		if curl -sf --max-time 20 -D /tmp/y2s-wtail.h "http://$host:8080/tail?log=$lt&dmesg=$dt" -o /tmp/y2s-wtail.b; then
			nl=$(sed -n 's/^X-Log-Total: \([0-9]*\).*/\1/p' /tmp/y2s-wtail.h | tr -d '\r')
			nd=$(sed -n 's/^X-Dmesg-Total: \([0-9]*\).*/\1/p' /tmp/y2s-wtail.h | tr -d '\r')
			if [ "$first" = 1 ]; then tail -c 4000 /tmp/y2s-wtail.b; first=0; else cat /tmp/y2s-wtail.b; fi
			lt=${nl:-$lt}
			dt=${nd:-$dt}
		fi
		sleep 1
	done
}
case "${1:-}" in
wlog) cmd_wlog ;;
wtail) cmd_wtail ;;
ulog) cmd_ulog ;;
mlog) cmd_mlog ;;
uflash) cmd_uflash ;;
uboot) cmd_uboot ;;
hflash) cmd_hflash ;;
hlogo) cmd_hlogo ;;
ureboot) cmd_ureboot ;;
standalone) cmd_standalone ;;
kernel) cmd_kernel ;;
payload) cmd_payload "${2:-}" ;;
cmdline) cmd_cmdline ;;
pack) cmd_pack ;;
flash) cmd_flash ;;
log) cmd_log ;;
ship) cmd_payload "${2:-}"; cmd_pack; cmd_flash ;;
*) sed -n '2,12p' "$0"; exit 1 ;;
esac
