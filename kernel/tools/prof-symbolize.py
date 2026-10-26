#!/usr/bin/env python3
"""
Names the libredwoodair.dylib addresses in a run's `prof:` lines (redwoodmetal.m's sampling
profiler, com.redwood.metal.prof). The library on the phone has its local symbols stripped
(mk-mesa.sh: llvm-strip -x), so the profiler prints offsets; the build's own unstripped copy has
the same layout and names them. Also adds up the samples by function, which the profiler could
not do for this library.

    tools/prof-symbolize.py xnulog.txt [unstripped libredwoodair.dylib]
"""
import bisect
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def unstripped():
    hits = glob.glob(os.path.join(HERE, '..', 'xnu-deps', 'build', 'mesa', 'build-*', '**', 'libredwoodair.dylib'),
                     recursive=True)
    if not hits:
        sys.exit('no unstripped libredwoodair.dylib under xnu-deps/build/mesa/build-*: pass one')
    return max(hits, key=os.path.getmtime)


def symbols(lib):
    out = subprocess.run(['nm', '-n', lib], capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in 'tT':
            addrs.append(int(parts[0], 16))
            names.append(parts[2].lstrip('_'))
    return addrs, names


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    lib = sys.argv[2] if len(sys.argv) > 2 else unstripped()
    addrs, names = symbols(lib)

    def name(off):
        i = bisect.bisect_right(addrs, off) - 1
        return '%s+%#x' % (names[i], off - addrs[i]) if i >= 0 else '?'

    def fn(off):
        i = bisect.bisect_right(addrs, off) - 1
        return names[i] if i >= 0 else '?'

    ref = re.compile(r'libredwoodair\.dylib\+0x([0-9a-f]+)')
    air_line = re.compile(r'prof: air 0x([0-9a-f]+) (\d+)')
    total = {}
    with open(sys.argv[1], errors='replace') as f:
        for line in f:
            if 'prof:' not in line and not line.startswith('    '):
                continue
            m = air_line.search(line)
            if m:
                k = fn(int(m.group(1), 16))
                total[k] = total.get(k, 0) + int(m.group(2))
                continue
            print(ref.sub(lambda m: 'air:' + name(int(m.group(1), 16)), line.rstrip('\n')))
    if total:
        print('\nlibredwoodair by function (its own samples, from the `prof: air` lines):')
        for k, v in sorted(total.items(), key=lambda kv: -kv[1])[:30]:
            print('%7d  %s' % (v, k))


if __name__ == '__main__':
    main()
