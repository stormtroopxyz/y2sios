#!/usr/bin/env python3
"""Read Apple's code where mali2metal's crash records point (CRASH records carry "[cache 0x...]").

usage: tools/dsc.py ADDRESS [--before N] [--count N] [--cache PATH]

Disassembles the iOS 16.5 shared cache (ios165/20F66__iPhone10,3_6/dyld_shared_cache_arm64 and its .NN
sub-caches, as the cache was built: unslid) around ADDRESS: N instructions before it and COUNT from it.
Each sub-cache has its own mapping table, so an address is found in whichever file maps it. Where an
instruction pair loads a selector (adrp + ldr x1 from __objc_selrefs), the selector's name is shown, and
the name of the image a branch lands in, so a call into an object of ours can be read as a message.
Needs capstone (pip3 install --user capstone).
"""
import argparse
import glob
import os
import struct
import sys

import capstone

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.join(HERE, '..', 'ios165', '20F66__iPhone10,3_6', 'dyld_shared_cache_arm64')


class Cache:
    def __init__(self, base):
        self.maps = []          # (address, size, file offset, file)
        self.images = []        # (address, path)
        for path in [base] + sorted(p for p in glob.glob(base + '.*') if not p.endswith(('.symbols', '.map'))):
            f = open(path, 'rb')
            head = f.read(0x200)
            if not head.startswith(b'dyld_v1'):
                continue
            moff, mcount = struct.unpack_from('<II', head, 0x10)
            f.seek(moff)
            for _ in range(mcount):
                address, size, fileoff, _maxp, _initp = struct.unpack('<QQQII', f.read(32))
                self.maps.append((address, size, fileoff, f))
            if path == base:
                ioff, icount = struct.unpack_from('<II', head, 0x1c0)
                f.seek(ioff)
                entries = [struct.unpack('<QQQII', f.read(32)) for _ in range(icount)]
                for address, _mod, _inode, pathoff, _pad in entries:
                    f.seek(pathoff)
                    self.images.append((address, f.read(256).split(b'\0', 1)[0].decode()))
        self.images.sort()

    def read(self, address, size):
        for a, s, off, f in self.maps:
            if a <= address < a + s:
                f.seek(off + address - a)
                return f.read(min(size, a + s - address))
        return None

    def u64(self, address):
        b = self.read(address, 8)
        return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None

    def cstring(self, address):
        b = self.read(address, 200)
        if not b:
            return None
        s = b.split(b'\0', 1)[0]
        return s.decode('ascii', 'replace') if s and all(32 <= c < 127 for c in s) else None

    def stub_selector(self, md, address):
        """The selector of an objc_msgSend stub (adrp x1; ldr x1, [x1, #off]; ... br x16), else None."""
        code = self.read(address, 8)
        if not code or len(code) < 8:
            return None
        insns = list(md.disasm(code, address))
        if len(insns) == 2 and insns[0].mnemonic == 'adrp' and insns[0].op_str.startswith('x1,') and \
                insns[1].mnemonic == 'ldr' and insns[1].op_str.startswith('x1, [x1'):
            page = int(insns[0].op_str.split('#')[1], 0)
            off = int(insns[1].op_str.split('#')[1].rstrip(']'), 0)
            return self.cstring(self.pointer(self.u64(page + off)))
        return None

    def image_of(self, address):
        name = None
        for a, path in self.images:
            if a > address:
                break
            name = path
        return os.path.basename(name) if name else '?'

    def pointer(self, raw):
        """A pointer as stored in the cache's data. This is an arm64 cache (the iPhone X's A11, not arm64e):
        slide info v2, the delta to the next pointer in bits 40-55 and the target in the low 40 bits."""
        if raw is None:
            return None
        return raw & 0xffffffffff


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('address', type=lambda x: int(x, 0))
    ap.add_argument('--before', type=int, default=24)
    ap.add_argument('--count', type=int, default=16)
    ap.add_argument('--cache', default=DEFAULT)
    args = ap.parse_args()
    cache = Cache(args.cache)
    start = args.address - 4 * args.before
    code = cache.read(start, 4 * (args.before + args.count))
    if code is None:
        sys.exit('%#x is not mapped by any sub-cache of %s' % (args.address, args.cache))
    print('%s, around %#x' % (cache.image_of(args.address), args.address))
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = True
    pages = {}
    for insn in md.disasm(code, start):
        note = ''
        ops = insn.op_str
        if insn.mnemonic == 'adrp':
            reg = ops.split(',')[0]
            pages[reg] = int(ops.split('#')[1], 0)
        elif insn.mnemonic in ('ldr', 'add') and '#' in ops:
            parts = [p.strip() for p in ops.replace('[', '').replace(']', '').split(',')]
            if len(parts) >= 3 and parts[1] in pages:
                target = pages[parts[1]] + int(parts[2].lstrip('#'), 0)
                if insn.mnemonic == 'ldr':
                    p = cache.pointer(cache.u64(target))
                    s = cache.cstring(p) if p else None
                    note = '  ; [%#x] -> %s' % (target, ('"%s"' % s) if s else ('%#x' % p if p else '?'))
                else:
                    s = cache.cstring(target)
                    note = '  ; %#x%s' % (target, (' "%s"' % s) if s else '')
        elif insn.mnemonic in ('bl', 'b') and ops.startswith('#'):
            target = int(ops[1:], 0)
            sel = cache.stub_selector(md, target)
            note = '  ; [%s]' % sel if sel else '  ; %s' % cache.image_of(target)
        mark = '=>' if insn.address == args.address else '  '
        print('%s %#x  %-6s %s%s' % (mark, insn.address, insn.mnemonic, ops, note))


if __name__ == '__main__':
    main()
