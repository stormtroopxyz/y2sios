#!/usr/bin/env python3
"""Find a C string in one image of the iOS 16.5 shared cache and the code that loads its address.

usage: tools/dsc_xref.py IMAGE-SUFFIX TEXT

IMAGE-SUFFIX names the image by the end of its path (Metal.framework/Metal); TEXT is searched in its
__TEXT,__cstring. For every match, the adrp+add pairs in its __TEXT,__text that form the string's address
are printed, so `tools/dsc.py ADDRESS --before N` can show the code around each use.
"""
import struct
import sys

import capstone

sys.path.insert(0, __import__('os').path.dirname(__file__))
from dsc import DEFAULT, Cache  # noqa: E402


def sections(cache, header):
    """(segment, section) -> (address, size) from the image's mach-o load commands."""
    head = cache.read(header, 32)
    ncmds, sizeofcmds = struct.unpack_from('<II', head, 16)
    cmds = cache.read(header + 32, sizeofcmds)
    out, at = {}, 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', cmds, at)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects = struct.unpack_from('<I', cmds, at + 64)[0]
            for i in range(nsects):
                s = at + 72 + 80 * i
                sect = cmds[s:s + 16].split(b'\0', 1)[0].decode()
                seg = cmds[s + 16:s + 32].split(b'\0', 1)[0].decode()
                addr, sz = struct.unpack_from('<QQ', cmds, s + 32)
                out[(seg, sect)] = (addr, sz)
        at += size
    return out


def main():
    suffix, text = sys.argv[1], sys.argv[2].encode()
    cache = Cache(DEFAULT)
    header = next((a for a, p in cache.images if p.endswith(suffix)), None)
    if header is None:
        sys.exit('no image ending in %s' % suffix)
    secs = sections(cache, header)
    caddr, csize = secs[('__TEXT', '__cstring')]
    blob = cache.read(caddr, csize)
    hits, at = [], 0
    while True:
        i = blob.find(text, at)
        if i < 0:
            break
        start = blob.rfind(b'\0', 0, i) + 1
        hits.append(caddr + start)
        print('string 0x%x: %s' % (caddr + start, blob[start:blob.find(b'\0', i)].decode('ascii', 'replace')))
        at = i + 1
    taddr, tsize = secs[('__TEXT', '__text')]
    code = cache.read(taddr, tsize)
    want = set(hits)
    for off in range(0, len(code) - 8, 4):
        w0, w1 = struct.unpack_from('<II', code, off)
        if (w0 & 0x9f000000) != 0x90000000 or (w1 & 0xffc00000) != 0x91000000:
            continue
        if (w0 & 0x1f) != ((w1 >> 5) & 0x1f):
            continue
        pc = taddr + off
        imm = ((w0 >> 29) & 3) | (((w0 >> 5) & 0x7ffff) << 2)
        if imm & (1 << 20):
            imm -= 1 << 21
        target = (pc & ~0xfff) + (imm << 12) + ((w1 >> 10) & 0xfff)
        if target in want:
            print('  loaded at 0x%x (string 0x%x)' % (pc, target))


if __name__ == '__main__':
    capstone  # (dsc.py's dependency; imported so a missing one fails here, by name)
    main()
