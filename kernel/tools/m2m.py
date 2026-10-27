#!/usr/bin/env python3
"""mali2metal's host side: fetch the phone's Metal channel, keep each run, report it, compare runs.

usage:
  tools/m2m.py fetch [--label L]          GET /m2m from the phone, saved as logs/m2m/<time>-<label>.m2m
  tools/m2m.py report RUN [--prev RUN]    what a run says; with --prev, what changed (default: the run before)
  tools/m2m.py tour                       start the standard tour on the phone (rwlogd GET /tour)
  tools/m2m.py wait-tour [--timeout S]    wait until the channel has "tour done" (or a client crashed)
  tools/m2m.py notify NAME                post com.redwood.metal.NAME on the phone (summary, prof, trace, ...)
  tools/m2m.py wait PATTERN [--timeout S] wait until a record after the latest "tour start" matches PATTERN
  tools/m2m.py tail                       follow the channel live

The phone: $Y2S_IP, else the Bonjour name HackiOS.local. The record format and the categories are in
PLANS/10-mali2metal.md; a record is

  <seq> <seconds.millis> <sev> <CAT> <process>[<pid>] <message>
"""
import argparse
import glob
import os
import re
import sys
import time
import urllib.request
from collections import Counter, OrderedDict, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(HERE, '..', 'logs', 'm2m')
RECORD = re.compile(r'^(\d+) (\d+)\.(\d{3}) ([IWEF]) (\S+)\s+(.+?)\[(\d+)\] (.*)$')
SEVERITY = {'I': 0, 'W': 1, 'E': 2, 'F': 3}


def host():
    return os.environ.get('Y2S_IP') or 'HackiOS.local'


def get(path, timeout=60):
    with urllib.request.urlopen('http://%s:8080%s' % (host(), path), timeout=timeout) as r:
        return r.read().decode('utf-8', 'replace'), dict(r.headers)


class Record:
    __slots__ = ('seq', 't', 'sev', 'cat', 'proc', 'pid', 'msg', 'step')

    def __init__(self, m):
        self.seq = int(m.group(1))
        self.t = int(m.group(2)) + int(m.group(3)) / 1000.0
        self.sev, self.cat, self.proc, self.pid, self.msg = m.group(4), m.group(5), m.group(6), int(m.group(7)), m.group(8)
        self.step = None


def parse(text):
    records, bad = [], 0
    for line in text.splitlines():
        m = RECORD.match(line)
        if m:
            records.append(Record(m))
        elif line.strip():
            bad += 1
    step = 'before the tour'
    for r in records:
        if r.cat == 'MARK':
            step = r.msg
        r.step = step
    return records, bad


def signature(r):
    """What a record says with its particulars taken out, so that repeats count as one problem."""
    s = re.sub(r'0x[0-9a-fA-F]+', 'H', r.msg)
    s = re.sub(r'\b[0-9a-f]{8,}\b', 'H', s)
    s = re.sub(r'\d+(\.\d+)?', 'N', s)
    s = re.sub(r' \[repeat N\]$', '', s)
    return '%s %s %s' % (r.sev, r.cat, s[:200])


def repeat_count(r):
    m = re.search(r'\[repeat (\d+)\]$', r.msg)
    return int(m.group(1)) if m else 1


def problems(records):
    """E/F and W records grouped by signature: count (repeats included), processes, first seq and step."""
    groups = OrderedDict()
    for r in records:
        if r.sev == 'I' or r.cat in ('MARK', 'PERF'):
            continue
        k = signature(r)
        g = groups.setdefault(k, {'count': 0, 'procs': Counter(), 'first': r, 'sev': r.sev})
        # RW_AGAIN reports on powers of two with the running count: the largest seen is the count
        g['count'] = max(g['count'] + 1, repeat_count(r))
        g['procs'][r.proc] += 1
    return groups


PERF = re.compile(r'perf: 5 s: (\d+) command buffers \(([\d.]+) a second\), (\d+) draws; commit ([\d.]+) ms each '
                  r'\(longest ([\d.]+)\), GPU wait ([\d.]+) ms each \(longest ([\d.]+)\)')
ALLOCS = re.compile(r'allocations: (\d+) buffers made \((\d+) KB, ([\d.]+) ms in the library\), (\d+) freed; '
                    r'(\d+) textures made \(([\d.]+) ms\), (\d+) freed')
PER_DRAW = re.compile(r'per draw: ([\d.]+) us \(Gallium ([\d.]+) us, \d+%\), (\d+) draws')
GPU = re.compile(r'gpu (\d+) ms: (\d+) fragment jobs (\d+) ms \(longest ([\d.]+)\), (\d+) vertex/tiler/compute jobs (\d+) ms '
                 r'\(longest ([\d.]+)\); GPU idle (\d+) ms.*?GPU clock (\d+) MHz')


def perf(records):
    """Per process: the 5 s windows that drew something, averaged; the GPU's own windows."""
    per = defaultdict(list)
    gpu = []
    draw = defaultdict(lambda: [0.0, 0.0, 0])      # weighted by draws: total us, library us, draws
    alloc = defaultdict(lambda: [0.0] * 7)         # summed over the run: the ALLOCS fields
    for r in records:
        if r.cat != 'PERF':
            continue
        m = ALLOCS.search(r.msg)
        if m:
            a = alloc[r.proc]
            for i in range(7):
                a[i] += float(m.group(i + 1))
            continue
        m = PER_DRAW.search(r.msg)
        if m:
            n = int(m.group(3))
            d = draw[r.proc]
            d[0] += float(m.group(1)) * n
            d[1] += float(m.group(2)) * n
            d[2] += n
            continue
        m = PERF.search(r.msg)
        if m and int(m.group(1)) > 0:
            per[r.proc].append(tuple(float(x) for x in m.groups()))
            continue
        m = GPU.search(r.msg)
        if m:
            gpu.append(tuple(float(x) for x in m.groups()))
    out = {}
    for proc, rows in per.items():
        n = len(rows)
        out[proc] = {
            'windows': n,
            'cb_per_s': sum(x[1] for x in rows) / n,
            'draws_per_cb': sum(x[2] for x in rows) / max(1, sum(x[0] for x in rows)),
            'commit_ms': sum(x[3] for x in rows) / n,
            'commit_max': max(x[4] for x in rows),
            'gpu_wait_ms': sum(x[5] for x in rows) / n,
            'gpu_wait_max': max(x[6] for x in rows),
            'draw_us': draw[proc][0] / draw[proc][2] if draw[proc][2] else 0.0,
            'draw_lib_us': draw[proc][1] / draw[proc][2] if draw[proc][2] else 0.0,
            # allocations a second over the windows that drew, and the library's ms a second on them
            'allocs_per_s': (alloc[proc][0] + alloc[proc][4]) / (5.0 * n),
            'alloc_ms_per_s': (alloc[proc][2] + alloc[proc][5]) / (5.0 * n),
        }
    if gpu:
        n = len(gpu)
        out['(GPU)'] = {
            'windows': n,
            'busy_pct': 100.0 * sum(x[2] + x[5] for x in gpu) / max(1, sum(x[0] for x in gpu)),
            'idle_pct': 100.0 * sum(x[7] for x in gpu) / max(1, sum(x[0] for x in gpu)),
            'frag_max_ms': max(x[3] for x in gpu),
            'mhz': sum(x[8] for x in gpu) / n,
        }
    return out


SLOW_DRAW = re.compile(r'^slow draw: ([\d.]+) ms \(Gallium ([\d.]+) ms\) (\S+)')
SLOW_COMMIT = re.compile(r'^slow: ([\d.]+) ms commit \(([\d.]+) ms GPU wait\), (\d+) draws: (\S+?)(?: x\d+)?(?:,|$)')


SLOW_COMPLETION = re.compile(r'^slow completion: ([\d.]+) ms .*?queued ([\d.]+) ms .*?GPU fence ([\d.]+) ms, (\d+) completed '
                             r'handlers? ([\d.]+) ms')
# since run 12 the old "queued" is split: commit bookkeeping, scheduled handlers, time on the fence queue
SLOW_COMPLETION_SPLIT = re.compile(r'^slow completion: ([\d.]+) ms .*?commit bookkeeping ([\d.]+) ms, (\d+) scheduled '
                                   r'handlers? ([\d.]+) ms \(([^)]*)\), on the fence queue ([\d.]+) ms, GPU fence '
                                   r'([\d.]+) ms, (\d+) completed handlers? ([\d.]+) ms')


def hotspots(records):
    """W PERF records summed: per (process, kind, what) the count, total ms and the worst."""
    hot = {}
    for r in records:
        if r.cat != 'PERF' or r.sev != 'W':
            continue
        m = SLOW_DRAW.match(r.msg)
        key, ms = None, 0.0
        if m:
            key, ms = (r.proc, 'draw', m.group(3)), float(m.group(1))
        else:
            m = SLOW_COMMIT.match(r.msg)
            if m:
                key, ms = (r.proc, 'commit', '%s draws, first %s' % (m.group(3), m.group(4))), float(m.group(1))
                key = (r.proc, 'commit', 'commits of command buffers')
        if key is None:
            continue
        n, total, worst = hot.get(key, (0, 0.0, 0.0))
        hot[key] = (n + 1, total + ms, max(worst, ms))
    return hot


def load(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        return parse(f.read())


def runs():
    return sorted(glob.glob(os.path.join(RUNS, '*.m2m')))


def report(path, prev_path=None, out=sys.stdout):
    records, bad = load(path)
    w = out.write
    w('== mali2metal run %s\n' % os.path.basename(path))
    if not records:
        w('no records\n')
        return
    span = records[-1].t - records[0].t
    sev = Counter(r.sev for r in records)
    w('%d records over %.0f s (seq %d..%d%s): %d F, %d E, %d W, %d I\n' % (
        len(records), span, records[0].seq, records[-1].seq,
        '; the ring had lost the start' if records[0].seq != 1 else '', sev['F'], sev['E'], sev['W'], sev['I']))
    if bad:
        w('%d lines are not records (a writer bug)\n' % bad)
    logs = sum(1 for r in records if r.cat == 'LOG')
    if logs:
        w('%d records without a category (I LOG: a writer that does not use m2m_log)\n' % logs)
    clients = Counter(r.proc for r in records if r.cat == 'DEV' and r.msg.startswith('client: '))
    w('Metal clients: %s\n' % (', '.join('%s%s' % (p, ' x%d (restarted)' % n if n > 1 else '')
                                         for p, n in clients.items()) or 'none registered'))
    marks = [r for r in records if r.cat == 'MARK']
    if marks:
        w('tour: %s\n' % ' -> '.join('%s@%.0fs' % (m.msg, m.t) for m in marks))

    crashes = [r for r in records if r.cat == 'CRASH']
    w('\n-- crashes: %d\n' % len(crashes))
    for r in crashes:
        w('  %.1fs %s[%d] %s  (during: %s)\n' % (r.t, r.proc, r.pid, r.msg, r.step))
    faults = [r for r in records if r.cat == 'GPU' and r.sev in 'EF']
    w('-- GPU faults: %d\n' % len(faults))
    for r in faults[:8]:
        w('  %.1fs %s[%d] %s\n' % (r.t, r.proc, r.pid, r.msg))

    groups = problems(records)
    for level, title in (('F', 'fatal'), ('E', 'errors'), ('W', 'declined / degraded')):
        rows = [(k, g) for k, g in groups.items() if g['sev'] == level and g['first'].cat not in ('CRASH',)]
        if level in 'F' and not rows:
            continue
        w('\n-- %s: %d kinds, %d records\n' % (title, len(rows), sum(g['count'] for _, g in rows)))
        for k, g in sorted(rows, key=lambda kg: -kg[1]['count'])[:25]:
            f = g['first']
            w('  %5dx %-5s %s  [%s; first #%d, %s]\n' % (g['count'], f.cat, f.msg[:150],
                                                        ', '.join(g['procs']), f.seq, f.step))

    steps = OrderedDict()
    for r in records:
        if r.sev in 'EFW' and r.cat not in ('MARK', 'PERF'):
            steps.setdefault(r.step, Counter())[r.sev] += 1
    if marks:
        w('\n-- per tour step (F/E/W records, performance excluded)\n')
        for m in ['before the tour'] + [m.msg for m in marks]:
            c = steps.get(m, Counter())
            w('  %-60s %3d F %4d E %4d W\n' % (m[:60], c['F'], c['E'], c['W']))

    hot = hotspots(records)
    if hot:
        w('\n-- where the time goes (slow draws and commits, by what they drew)\n')
        for (proc, kind, label), (n, total, worst) in sorted(hot.items(), key=lambda kv: -kv[1][1])[:15]:
            w('  %8.1f ms in %4d %-6s %5.2f ms avg, worst %5.1f  %s: %s\n' % (total, n, kind, total / n, worst, proc, label[:110]))

    slow = defaultdict(list)
    split = defaultdict(list)
    for r in records:
        if r.cat != 'PERF':
            continue
        m = SLOW_COMPLETION_SPLIT.match(r.msg)
        if m:
            split[r.proc].append(tuple(float(m.group(i)) for i in (1, 2, 4, 6, 7, 9)) + (m.group(5),))
            continue
        m = SLOW_COMPLETION.match(r.msg)
        if m:
            slow[r.proc].append(tuple(float(m.group(i)) for i in (1, 2, 3, 5)))
    if slow or split:
        w('\n-- slow completions (over 100 ms from commit to completed handlers): average split\n')
        for proc, rows in slow.items():
            n = len(rows)
            w('  %-14s %4d, longest %.0f ms: queued %.1f ms, GPU fence %.1f ms, handlers %.1f ms\n' % (
                proc, n, max(x[0] for x in rows), sum(x[1] for x in rows) / n, sum(x[2] for x in rows) / n,
                sum(x[3] for x in rows) / n))
        for proc, rows in split.items():
            n = len(rows)
            avg = [sum(x[i] for x in rows) / n for i in range(1, 6)]
            w('  %-14s %4d, longest %.0f ms: bookkeeping %.1f, scheduled handlers %.1f (%s), fence queue %.1f, '
              'GPU fence %.1f, completed handlers %.1f ms\n' % (
                  proc, n, max(x[0] for x in rows), avg[0], avg[1], ', '.join(sorted({x[6] for x in rows})),
                  avg[2], avg[3], avg[4]))

    p = perf(records)
    w('\n-- performance (5 s windows that drew)\n')
    for proc, v in p.items():
        if proc == '(GPU)':
            w('  GPU: %d windows, busy %.1f%%, idle %.1f%%, longest fragment job %.1f ms, %.0f MHz\n' % (
                v['windows'], v['busy_pct'], v['idle_pct'], v['frag_max_ms'], v['mhz']))
        else:
            w('  %-14s %3d windows: %.1f command buffers/s, %.0f draws each, commit %.2f ms (max %.1f), '
              'GPU wait %.2f ms (max %.1f); a draw %.1f us (library %.1f)\n' % (
                  proc, v['windows'], v['cb_per_s'], v['draws_per_cb'], v['commit_ms'], v['commit_max'], v['gpu_wait_ms'],
                  v['gpu_wait_max'], v['draw_us'], v['draw_lib_us']))
    sums = [r for r in records if r.cat == 'SUM']
    if sums:
        w('\n-- totals (SUM)\n')
        for r in sums:
            w('  %s[%d] %s\n' % (r.proc, r.pid, r.msg.replace('==== METAL SUMMARY: ', '')[:200]))

    if prev_path:
        compare(prev_path, records, groups, p, out)


def compare(prev_path, records, groups, p, out):
    w = out.write
    prev, _ = load(prev_path)
    pg = problems(prev)
    pp = perf(prev)
    w('\n== compared with %s\n' % os.path.basename(prev_path))
    pc = Counter(r.sev for r in prev)
    c = Counter(r.sev for r in records)
    w('F %d -> %d, E %d -> %d, W %d -> %d\n' % (pc['F'], c['F'], pc['E'], c['E'], pc['W'], c['W']))
    new = [k for k in groups if k not in pg]
    gone = [k for k in pg if k not in groups]
    w('-- new problems: %d\n' % len(new))
    for k in new[:20]:
        g = groups[k]
        w('  %5dx %s\n' % (g['count'], k[:170]))
    w('-- gone: %d\n' % len(gone))
    for k in gone[:20]:
        w('  %5dx %s\n' % (pg[k]['count'], k[:170]))
    changed = [(k, pg[k]['count'], groups[k]['count']) for k in groups if k in pg and pg[k]['count'] != groups[k]['count']]
    if changed:
        w('-- counts changed: %d\n' % len(changed))
        for k, a, b in sorted(changed, key=lambda x: -abs(x[2] - x[1]))[:10]:
            w('  %5d -> %-5d %s\n' % (a, b, k[:150]))
    w('-- performance\n')
    for proc in sorted(set(p) | set(pp)):
        a, b = pp.get(proc), p.get(proc)
        if not a or not b:
            w('  %s: %s\n' % (proc, 'new' if b else 'gone'))
            continue
        keys = ['busy_pct', 'idle_pct', 'frag_max_ms', 'mhz'] if proc == '(GPU)' else \
            ['cb_per_s', 'draws_per_cb', 'commit_ms', 'commit_max', 'gpu_wait_ms', 'draw_us', 'draw_lib_us',
             'allocs_per_s', 'alloc_ms_per_s']
        w('  %-14s %s\n' % (proc, ', '.join('%s %.2f -> %.2f' % (k, a[k], b[k]) for k in keys)))


def cmd_fetch(args):
    text, headers = get('/m2m')
    os.makedirs(RUNS, exist_ok=True)
    name = time.strftime('%Y%m%d-%H%M%S') + ('-' + args.label if args.label else '') + '.m2m'
    path = os.path.join(RUNS, name)
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    print(path)
    return path


def cmd_report(args):
    path = args.run or (runs()[-1] if runs() else None)
    if path is None:
        sys.exit('no runs in %s' % RUNS)
    prev = args.prev
    if prev is None and not args.no_prev:
        older = [r for r in runs() if os.path.abspath(r) != os.path.abspath(path) and r < path]
        prev = older[-1] if older else None
    report(path, prev)


def cmd_tour(_args):
    text, _ = get('/tour', timeout=10)
    print(text.strip())


def cmd_wait_tour(args):
    deadline = time.time() + args.timeout
    start = None
    while time.time() < deadline:
        try:
            text, headers = get('/m2m', timeout=30)
        except OSError as e:
            print('wait-tour: %s' % e, file=sys.stderr)
            time.sleep(5)
            continue
        records, _ = parse(text)
        starts = [r for r in records if r.cat == 'MARK' and r.msg == 'tour start']
        if starts:
            start = starts[-1].seq
            if any(r.cat == 'MARK' and r.msg == 'tour done' and r.seq > start for r in records):
                print('tour done')
                return 0
            crashed = [r for r in records if r.cat == 'CRASH' and r.seq > start]
            if crashed and time.time() + 20 > deadline:
                print('tour: a client crashed: %s' % crashed[-1].msg)
                return 1
        time.sleep(5)
    print('wait-tour: no "tour done" in %d s%s' % (args.timeout, '' if start else ' (the tour never started)'))
    return 1


def cmd_notify(args):
    text, _ = get('/notify?name=com.redwood.metal.%s' % args.name, timeout=10)
    print(text.strip())


def cmd_wait(args):
    pattern = re.compile(args.pattern)
    deadline = time.time() + args.timeout
    while time.time() < deadline:
        try:
            records, _ = parse(get('/m2m', timeout=30)[0])
        except OSError as e:
            print('wait: %s' % e, file=sys.stderr)
            time.sleep(5)
            continue
        starts = [r.seq for r in records if r.cat == 'MARK' and r.msg == 'tour start']
        after = starts[-1] if starts else 0
        if any(r.seq > after and pattern.search(r.msg) for r in records):
            return 0
        time.sleep(5)
    print('wait: no record matching %r in %d s' % (args.pattern, args.timeout))
    return 1


def cmd_tail(_args):
    at = 0
    while True:
        try:
            text, headers = get('/m2m?from=%d' % at if at else '/m2m', timeout=30)
            at = int(headers.get('X-M2M-Total', at))
            sys.stdout.write(text)
            sys.stdout.flush()
        except OSError as e:
            print('tail: %s' % e, file=sys.stderr)
        time.sleep(2)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    f = sub.add_parser('fetch')
    f.add_argument('--label', default='')
    r = sub.add_parser('report')
    r.add_argument('run', nargs='?')
    r.add_argument('--prev')
    r.add_argument('--no-prev', action='store_true')
    sub.add_parser('tour')
    t = sub.add_parser('wait-tour')
    t.add_argument('--timeout', type=int, default=240)
    n = sub.add_parser('notify')
    n.add_argument('name')
    w = sub.add_parser('wait')
    w.add_argument('pattern')
    w.add_argument('--timeout', type=int, default=120)
    sub.add_parser('tail')
    args = ap.parse_args()
    rc = {'fetch': cmd_fetch, 'report': cmd_report, 'tour': cmd_tour, 'wait-tour': cmd_wait_tour,
          'notify': cmd_notify, 'wait': cmd_wait, 'tail': cmd_tail}[args.cmd](args)
    sys.exit(rc if isinstance(rc, int) else 0)


if __name__ == '__main__':
    main()
