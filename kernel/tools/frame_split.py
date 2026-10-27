#!/usr/bin/env python3
"""Where a frame's time goes on the way to the panel, from the display driver's touch_lat=1 lines
(iokit/Kernel/MiniIOMobileFramebuffer.cpp): `tl: s ID at US` when the window server hands a swap over and
`tl: v ID ready US shown US done US` when the GPU had finished it, its beat came and it was on the panel.
(tools/frames.py is the touch-to-screen latency one.)

usage: tools/frame_split.py LOG [--gap MS]

Only frames in runs of animation count (a swap less than --gap ms, default 50, after the one before): an
idle screen's frames are spaced by having nothing to draw. Per frame: the interval between swaps (the frame
rate), the GPU (swap to ready), the beat (ready to shown), and the window server's own time from the frame
before being on the panel to this swap (shown(n-1) to s(n)). Then the driver's `fb_swap:` lines summed.
"""
import bisect
import re
import statistics
import sys


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p / 100))] if xs else 0


def main():
    gap = float(sys.argv[sys.argv.index('--gap') + 1]) if '--gap' in sys.argv else 50.0
    s, v, swap, vs = {}, {}, [0, 0, 0, 0], []
    for line in open(sys.argv[1], errors='replace'):
        m = re.search(r'tl: s (\d+) at (\d+)', line)
        if m:
            s[int(m.group(1))] = int(m.group(2))
            continue
        m = re.search(r'tl: v (\d+) ready (\d+) shown (\d+) done (\d+)', line)
        if m:
            v[int(m.group(1))] = tuple(int(x) for x in m.groups()[1:])
            continue
        m = re.search(r'tl: vs \d+ due \d+ sent (\d+)', line)
        if m:
            vs.append(int(m.group(1)))
            continue
        m = re.search(r'fb_swap: .*?(\d+) SwapWaits, (\d+) of them blocked .*?(\d+) timed out; (\d+) swaps found', line)
        if m:
            for i in range(4):
                swap[i] += int(m.group(i + 1))
    ids = sorted(i for i in s if i in v)
    rows = []
    for a, b in zip(ids, ids[1:]):
        interval = (s[b] - s[a]) / 1000
        if interval <= 0 or interval > gap:
            continue
        ready, shown, _ = v[b]
        row = {'interval': interval, 'gpu': (ready - s[b]) / 1000, 'beat': (shown - ready) / 1000,
               'drawing': (s[b] - v[a][1]) / 1000}
        # the window server's part split at the vsync it was told of last before the swap
        k = bisect.bisect_right(vs, s[b]) - 1
        if k >= 0 and vs[k] >= v[a][1]:
            row['to_vsync'] = (vs[k] - v[a][1]) / 1000
            row['vsync_swap'] = (s[b] - vs[k]) / 1000
        rows.append(row)
    if not rows:
        sys.exit('no frames in animation runs')
    iv = [r['interval'] for r in rows]
    print('%d frames in animation runs (of %d swaps)' % (len(rows), len(ids)))
    print('  frame rate: median %.1f fps (interval median %.1f ms, p90 %.1f ms)' % (
        1000 / statistics.median(iv), statistics.median(iv), pct(iv, 90)))
    for key, what in (('drawing', 'previous frame on the panel -> this swap'),
                      ('to_vsync', '  of it: on the panel -> vsync sent'),
                      ('vsync_swap', '  of it: vsync sent -> this swap'),
                      ('gpu', 'swap -> GPU done'), ('beat', 'GPU done -> on the panel')):
        xs = [r[key] for r in rows if key in r]
        if not xs:
            continue
        print('  %-42s median %6.2f ms  p90 %6.2f ms' % (what, statistics.median(xs), pct(xs, 90)))
    hist = {}
    for x in iv:
        k = min(int(x / 8.33), 6)
        hist[k] = hist.get(k, 0) + 1
    print('  intervals in 120 Hz periods (8.33 ms): ' + ', '.join(
        '%s%d: %d' % ('>=' if k == 6 else '', k + 1, hist[k]) for k in sorted(hist)))
    if any(swap):
        print('  fb_swap: %d SwapWaits, %d blocked, %d timed out; %d swaps would tear' % tuple(swap))


if __name__ == '__main__':
    main()
