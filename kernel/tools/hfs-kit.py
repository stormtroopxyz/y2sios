#!/usr/bin/env python3
"""
The S20+'s data volume (PLANS/09-standalone.md, P6): from the HFS+ that tools/mk-userdata-hfs.sh made with macOS's
newfs_hfs (head.img: its first 512 MB, catalog.img: the catalog B-tree's blocks, tail.img: its last 2 MB), a
"format kit" for the phone's init to write into userdata at the first boot (userland/src/init.c, var_hfs): every
block the new file system uses, as data where it is not zero and as a run of zeros where it is (so nothing Android
left there shows through: the bitmap, the B-trees' nodes, the journal).

    tools/hfs-kit.py [/tmp/hfsfmt] [out]          (default out: userland/hfs-kit.bin)

Format, little-endian: "HFSKIT01", u32 block size, u32 entries, u64 device bytes, u32 crc32 of what follows the
header, u32 reserved; entries { u64 first block, u32 blocks, u32 kind (0 zeros, 1 data) }; then the data of the
data entries, in order.
"""
import os
import struct
import sys
import zlib

BLOCK = 4096
DEVICE = 116253523968                   # userdata: 28382208 blocks of 4096
TOTAL = DEVICE // BLOCK
CATALOG_FIRST = 319333


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else '/tmp/hfsfmt'
    here = os.path.dirname(os.path.abspath(__file__))
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', 'userland', 'hfs-kit.bin')
    head = open(os.path.join(src, 'head.img'), 'rb').read()
    cat = open(os.path.join(src, 'catalog.img'), 'rb').read()
    tail = open(os.path.join(src, 'tail.img'), 'rb').read()
    tail_first = TOTAL - len(tail) // BLOCK

    vh = head[1024:1536]
    sig, = struct.unpack('>2s', vh[:2])
    bsize, total = struct.unpack('>II', vh[40:48])
    assert sig == b'HX' and bsize == BLOCK and total == TOTAL, (sig, bsize, total)
    cat_ext = struct.unpack('>II', vh[272 + 16:272 + 24])
    assert cat_ext == (CATALOG_FIRST, len(cat) // BLOCK), cat_ext

    def block(b):
        if b < len(head) // BLOCK:
            return head[b * BLOCK:(b + 1) * BLOCK]
        if CATALOG_FIRST <= b < CATALOG_FIRST + len(cat) // BLOCK:
            return cat[(b - CATALOG_FIRST) * BLOCK:(b - CATALOG_FIRST + 1) * BLOCK]
        if b >= tail_first:
            return tail[(b - tail_first) * BLOCK:(b - tail_first + 1) * BLOCK]
        raise SystemExit('block %d is used but not in the copies' % b)

    bitmap = head[BLOCK:BLOCK + 867 * BLOCK]
    used = [b for b in range(TOTAL) if (bitmap[b >> 3] >> (7 - (b & 7))) & 1]
    zero = bytes(BLOCK)
    entries, data = [], bytearray()
    for b in used:
        content = block(b)
        kind = 0 if content == zero else 1
        if entries and entries[-1][2] == kind and entries[-1][0] + entries[-1][1] == b:
            entries[-1][1] += 1
        else:
            entries.append([b, 1, kind])
        if kind:
            data += content
    body = b''.join(struct.pack('<QII', *e) for e in entries) + bytes(data)
    hdr = b'HFSKIT01' + struct.pack('<IIQII', BLOCK, len(entries), DEVICE, zlib.crc32(body) & 0xffffffff, 0)
    open(out, 'wb').write(hdr + body)
    zeros = sum(e[1] for e in entries if e[2] == 0)
    print('hfs-kit: %d blocks used by the file system: %d of data (%d KB), %d of zeros (%d MB), in %d runs -> %s (%d KB)'
          % (len(used), len(data) // BLOCK, len(data) >> 10, zeros, zeros * BLOCK >> 20, len(entries), out,
             (len(hdr) + len(body)) >> 10))


main()
