#!/usr/bin/env python3
"""
Names the libredwoodair.dylib addresses in a run's `prof:` lines (redwoodmetal.m's sampling
profiler, com.redwood.metal.prof). The library on the phone has its local symbols stripped
(mk-mesa.sh: llvm-strip -x), so the profiler prints offsets; the build's own unstripped copy has
the same layout and names them. Also adds up the samples by function, which the profiler could
not do for this library.

    tools/prof-symbolize.py RUN [unstripped libredwoodair.dylib]

RUN is a console log or a mali2metal run (logs/m2m/*.m2m). From the `prof: airstack` records (each a count
and the library's frames of the samples, innermost first, and `out:<image>` where the pc was outside it),
it also gives every library function's INCLUSIVE samples (time in it and in what it calls) and SELF
samples, and for the busiest the callees that take its time: the tree of where a draw's time goes.
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

    stack_line = re.compile(r'prof: airstack (\d+)((?: (?:0x[0-9a-f]+|out:\S+))+)')
    inclusive, self_, callees, outside = {}, {}, {}, {}
    stacks_total = 0
    ref = re.compile(r'libredwoodair\.dylib\+0x([0-9a-f]+)')
    air_line = re.compile(r'prof: air 0x([0-9a-f]+) (\d+)')
    total = {}
    with open(sys.argv[1], errors='replace') as f:
        for line in f:
            if 'prof:' not in line and not line.startswith('    '):
                continue
            m = stack_line.search(line)
            if m:
                count = int(m.group(1))
                frames = m.group(2).split()
                out = frames[0][4:] if frames[0].startswith('out:') else None
                chain = [fn(int(f, 16)) for f in frames if f.startswith('0x')]
                stacks_total += count
                for k in set(chain):
                    inclusive[k] = inclusive.get(k, 0) + count
                leaf = ('[%s]' % out) if out else chain[0]
                if out:
                    outside[chain[0]] = outside.get(chain[0], {})
                    outside[chain[0]][out] = outside[chain[0]].get(out, 0) + count
                else:
                    self_[chain[0]] = self_.get(chain[0], 0) + count
                # each caller -> its next callee (the leaf an outside image when the pc was there)
                for i in range(len(chain) - 1, 0, -1):
                    c = callees.setdefault(chain[i], {})
                    c[chain[i - 1]] = c.get(chain[i - 1], 0) + count
                if out:
                    c = callees.setdefault(chain[0], {})
                    c[leaf] = c.get(leaf, 0) + count
                continue
            m = air_line.search(line)
            if m:
                k = fn(int(m.group(1), 16))
                total[k] = total.get(k, 0) + int(m.group(2))
                continue
            print(ref.sub(lambda m: 'air:' + name(int(m.group(1), 16)), line.rstrip('\n')))
    if inclusive:
        print('\nlibredwoodair, inclusive (the function and what it calls) of %d samples in the library:' % stacks_total)
        print('%7s %6s %7s  %s' % ('incl', '%', 'self', 'function'))
        for k, v in sorted(inclusive.items(), key=lambda kv: -kv[1])[:40]:
            print('%7d %5.1f%% %7d  %s' % (v, 100.0 * v / stacks_total, self_.get(k, 0), k))
        print('\nwhere the busiest functions\' time goes (callees by samples; [image] = outside the library):')
        for k, v in sorted(inclusive.items(), key=lambda kv: -kv[1])[:15]:
            parts = sorted(callees.get(k, {}).items(), key=lambda kv: -kv[1])[:6]
            print('  %-40s %6d: %s' % (k[:40], v, ', '.join('%s %d' % (c, n) for c, n in parts) or '(all self)'))
    if total:
        print('\nlibredwoodair by function (its own samples, from the `prof: air` lines):')
        for k, v in sorted(total.items(), key=lambda kv: -kv[1])[:30]:
            print('%7d  %s' % (v, k))


if __name__ == '__main__':
    main()
