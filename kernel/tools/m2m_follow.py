#!/usr/bin/env python3
"""Follow the phone's mali2metal channel live and append every new record to a file, across reboots: what
is recorded before a hard reset (which leaves the loader no log) is kept on this side.

usage: tools/m2m_follow.py OUT [--host HOST] [--seconds 900]   (HOST: $Y2S_IP, else HackiOS.local)
"""
import argparse
import os
import time
import urllib.request


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--host', default=os.environ.get('Y2S_IP', 'HackiOS.local'))
    ap.add_argument('--seconds', type=int, default=900)
    a = ap.parse_args()
    end = time.time() + a.seconds
    at, boot = 0, 0
    with open(a.out, 'a') as out:
        while time.time() < end:
            try:
                with urllib.request.urlopen('http://%s:8080/m2m?from=%d' % (a.host, at), timeout=3) as r:
                    text = r.read().decode('utf-8', 'replace')
                    total = int(r.headers.get('X-M2M-Total', at))
            except Exception:
                time.sleep(1)
                continue
            if True:
                if total < at:                      # the phone started again: a new channel
                    boot += 1
                    at = 0
                    out.write('==== m2m_follow: the channel started again (boot %d) at %s ====\n' %
                              (boot, time.strftime('%H:%M:%S')))
                    continue
                if text:
                    out.write(text if text.endswith('\n') else text + '\n')
                    out.flush()
                at = total
            time.sleep(1)


if __name__ == '__main__':
    main()
