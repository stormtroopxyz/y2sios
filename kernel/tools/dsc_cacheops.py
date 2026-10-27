#!/usr/bin/env python3
"""Which images of the iOS 16.5 shared cache contain user cache-maintenance instructions (DC CVAU, DC CVAC,
DC CIVAC, DC CVAP, IC IVAU), counted per image and per instruction: what SCTLR_EL1.UCI=0 makes trap.

usage: tools/dsc_cacheops.py [--top N]
"""
import struct
import sys

import numpy as np

sys.path.insert(0, __import__('os').path.dirname(__file__))
from dsc import DEFAULT, Cache  # noqa: E402
from dsc_xref import sections  # noqa: E402

OPS = {0xD50B7520: 'ic ivau', 0xD50B7B20: 'dc cvau', 0xD50B7A20: 'dc cvac', 0xD50B7E20: 'dc civac',
       0xD50B7C20: 'dc cvap'}


def main():
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
    cache = Cache(DEFAULT)
    found = []
    for header, path in cache.images:
        try:
            secs = sections(cache, header)
        except Exception:
            continue
        text = secs.get(('__TEXT', '__text'))
        if not text:
            continue
        blob = cache.read(text[0], text[1])
        if not blob:
            continue
        words = np.frombuffer(blob[:len(blob) // 4 * 4], dtype='<u4')
        base = words & np.uint32(0xFFFFFFE0)
        counts = {}
        for enc, name in OPS.items():
            n = int(np.count_nonzero(base == enc))
            if n:
                counts[name] = n
        if counts:
            found.append((sum(counts.values()), path, counts))
    found.sort(reverse=True)
    print('%d images contain user cache maintenance' % len(found))
    for total, path, counts in found[:top]:
        print('%4d  %-70s %s' % (total, path, ', '.join('%s %d' % kv for kv in sorted(counts.items()))))


if __name__ == '__main__':
    struct  # (sections() reads with it)
    main()
