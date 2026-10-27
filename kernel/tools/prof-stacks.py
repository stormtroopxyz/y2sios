#!/usr/bin/env python3
"""Print the profiler's sampled library stacks (prof: airstack records) that pass through FUNCTION,
symbolized and summed by identical stack, innermost frame first.

usage: tools/prof-stacks.py RUN FUNCTION [unstripped libredwoodair.dylib] [--top N]
"""
import bisect
import collections
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('prof_symbolize', os.path.join(HERE, 'prof-symbolize.py'))
ps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ps)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    top = 12
    if '--top' in sys.argv:
        top = int(sys.argv[sys.argv.index('--top') + 1])
        args = [a for a in args if a != str(top)]
    if len(args) < 2:
        sys.exit(__doc__)
    run, func = args[0], args[1]
    lib = args[2] if len(args) > 2 else ps.unstripped()
    addrs, names = ps.symbols(lib)

    def name(tok):
        if tok.startswith('out:'):
            return '[' + tok[4:] + ']'
        a = int(tok, 16)
        i = bisect.bisect_right(addrs, a) - 1
        return names[i] if i >= 0 else tok

    line = re.compile(r'prof: airstack (\d+)((?: (?:0x[0-9a-f]+|out:\S+))+)')
    stacks = collections.Counter()
    with open(run, errors='replace') as f:
        for text in f:
            m = line.search(text)
            if not m:
                continue
            frames = [name(t) for t in m.group(2).split()]
            if func in frames:
                stacks[' <- '.join(frames)] += int(m.group(1))
    total = sum(stacks.values())
    print('%d samples through %s' % (total, func))
    for stack, n in stacks.most_common(top):
        print('%5d  %s' % (n, stack))


if __name__ == '__main__':
    main()
