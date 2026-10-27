#!/usr/bin/env python3
"""Name addresses in one image of the iOS 16.5 shared cache from the image's own export trie.

usage: tools/dsc_exports.py IMAGE-SUFFIX ADDRESS...
       tools/dsc_exports.py IMAGE-SUFFIX --name SYMBOL...   (the address of each exported symbol)
       tools/dsc_exports.py IMAGE-SUFFIX --list [SUBSTRING]  (every exported name, or those containing SUBSTRING)
"""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
from dsc import DEFAULT, Cache  # noqa: E402


def uleb(b, i):
    v = s = 0
    while True:
        c = b[i]
        i += 1
        v |= (c & 0x7f) << s
        s += 7
        if c < 0x80:
            return v, i


def main():
    suffix = sys.argv[1]
    by_name = len(sys.argv) > 2 and sys.argv[2] == '--name'
    listing = len(sys.argv) > 2 and sys.argv[2] == '--list'
    names = sys.argv[3:] if by_name or listing else []
    want = set() if by_name or listing else {int(a, 0) for a in sys.argv[2:]}
    cache = Cache(DEFAULT)
    header = next((a for a, p in cache.images if p.endswith(suffix)), None)
    if header is None:
        sys.exit('no image ending in %s' % suffix)
    ncmds, sizeofcmds = struct.unpack_from('<II', cache.read(header, 32), 16)
    cmds = cache.read(header + 32, sizeofcmds)
    at, linkedit, trie = 0, None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', cmds, at)
        if cmd == 0x19 and cmds[at + 8:at + 24].split(b'\0', 1)[0] == b'__LINKEDIT':
            vmaddr, _vmsize, fileoff = struct.unpack_from('<QQQ', cmds, at + 24)
            linkedit = (vmaddr, fileoff)
        elif cmd == 0x80000033:                       # LC_DYLD_EXPORTS_TRIE
            trie = struct.unpack_from('<II', cmds, at + 8)
        elif cmd in (0x22, 0x80000022):               # LC_DYLD_INFO(_ONLY): export_off, export_size
            trie = struct.unpack_from('<II', cmds, at + 40)
        at += size
    if not linkedit or not trie or not trie[1]:
        sys.exit('no export trie')
    data = cache.read(linkedit[0] + trie[0] - linkedit[1], trie[1])
    found = {}

    def walk(node, prefix):
        size, i = uleb(data, node)
        if size:
            flags, j = uleb(data, i)
            if not flags & 0x8:                       # not a re-export
                off, _ = uleb(data, j)
                found[header + off] = prefix
        i += size
        count = data[i]
        i += 1
        for _ in range(count):
            end = data.index(b'\0', i)
            edge = data[i:end].decode()
            child, i = uleb(data, end + 1)
            walk(child, prefix + edge)

    walk(0, '')
    if listing:
        for a, n in sorted(found.items(), key=lambda kv: kv[1]):
            if not names or any(x.lower() in n.lower() for x in names):
                print('0x%x %s' % (a, n))
        return
    if by_name:
        where = {n: a for a, n in found.items()}
        for n in names:
            a = where.get(n)
            print('%s %s' % (n, hex(a) if a is not None else '(not exported)'))
        return
    for a in sorted(want):
        print('0x%x %s' % (a, found.get(a, '(not an export)')))


if __name__ == '__main__':
    main()
