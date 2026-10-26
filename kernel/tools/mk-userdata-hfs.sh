#!/bin/sh
# The S20+'s data volume (PLANS/09-standalone.md, P6): a case-sensitive journaled HFS+ (HFSX) the exact size of
# its userdata partition (28382208 blocks of 4096 = 116253523968 bytes), made by macOS's own newfs_hfs on a sparse
# disk image -- nothing but files under /tmp/hfsfmt is touched. Its first 512 MB and its last 2 MB are copied out
# (head.img, tail.img); tools/hfs-kit.py takes the blocks the file system uses from them, for the phone's init
# to write into userdata at the first boot.
set -e
D=/tmp/hfsfmt
mkdir -p "$D"
cd "$D"
rm -f userdata.asif head.img tail.img catalog.img
diskutil image create blank --format ASIF --size 116253523968 --fs None userdata.asif
DEV=$(diskutil image attach --noMount userdata.asif | awk '{print $1}' | head -1)
case "$DEV" in /dev/disk*) ;; *) echo "attach gave no device: $DEV"; exit 1 ;; esac
echo "image attached as $DEV"
RDEV="/dev/r${DEV#/dev/}"
/sbin/newfs_hfs -J -s -b 4096 -v Data -U 0 -G 0 -M 755 "$RDEV"
dd if="$RDEV" of=head.img bs=1m count=512
dd if="$RDEV" of=tail.img bs=1m skip=110866
# (newfs_hfs puts the catalog B-tree in the metadata zone, ~1.2 GB in: blocks 319333..338276 at this size)
dd if="$RDEV" of=catalog.img bs=4096 skip=319333 count=18944
diskutil eject "$DEV" || hdiutil detach "$DEV"
ls -la "$D"
echo "done: $D/head.img, tail.img and catalog.img"
