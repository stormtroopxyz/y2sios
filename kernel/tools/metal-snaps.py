#!/usr/bin/env python3
"""Pictures of the screen out of a boot log (redwoodmetal.m -redwoodMaybeSnap, com.redwood.metal.snap).

usage: tools/metal-snaps.py [LOG] [OUTDIR]     (defaults: ../xnulog.txt, ../snaps)
Each `snap N: WxH ...` block becomes OUTDIR/snap-N.png. The data is raw DEFLATE (Foundation's
NSDataCompressionAlgorithmZlib) of RGB rows, base64 in lines of `snap N K: ...`.
"""
import base64
import os
import re
import struct
import sys
import zlib


def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data +
                struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, '..', '..', 'xnulog.txt')
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, '..', '..', 'snaps')
    os.makedirs(out, exist_ok=True)
    heads, parts = {}, {}
    for line in open(log, errors='replace'):
        m = re.search(r'snap (\d+): (\d+)x(\d+) of a .* (\d+) lines$', line.rstrip())
        if m:
            heads[int(m.group(1))] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
            continue
        m = re.search(r'snap (\d+) (\d+): ([A-Za-z0-9+/=]+)$', line.rstrip())
        if m:
            parts.setdefault(int(m.group(1)), {})[int(m.group(2))] = m.group(3)
    for n, (w, h, lines) in sorted(heads.items()):
        got = parts.get(n, {})
        if len(got) != lines:
            print('snap %d: %d of %d lines in the log, skipped' % (n, len(got), lines))
            continue
        rgb = zlib.decompress(base64.b64decode(''.join(got[i] for i in range(lines))), -15)
        path = os.path.join(out, 'snap-%d.png' % n)
        png(path, w, h, rgb)
        print('snap %d: %s (%dx%d)' % (n, path, w, h))


main()
