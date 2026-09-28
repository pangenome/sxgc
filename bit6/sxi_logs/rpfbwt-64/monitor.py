#!/usr/bin/env python3
"""Observe only this lane's launched processes and descendants."""
import json
import os
from pathlib import Path
import signal
import time

LOG = Path(__file__).resolve().parent
ROOTS = [7882, 7746, 11590]

def stat(pid):
    try:
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return dict(pid=pid, ppid=int(fields[1]), start=int(fields[19]),
                    virtual_bytes=int(fields[20]), rss_bytes=int(fields[21])*os.sysconf('SC_PAGE_SIZE'))
    except (OSError, ValueError):
        return None

roots = {pid: stat(pid) for pid in ROOTS}
roots = {pid: value['start'] for pid, value in roots.items() if value}
peak = 0
last = 0
with (LOG/'resources.jsonl').open('a', buffering=1) as log:
    while roots:
        procs = {int(p.name): stat(int(p.name)) for p in Path('/proc').iterdir() if p.name.isdigit()}
        procs = {pid: value for pid, value in procs.items() if value}
        roots = {pid: start for pid, start in roots.items() if pid in procs and procs[pid]['start'] == start}
        owned = set(roots)
        while True:
            expanded = owned | {pid for pid, s in procs.items() if s['ppid'] in owned}
            if expanded == owned:
                break
            owned = expanded
        rss = sum(procs[pid]['rss_bytes'] for pid in owned)
        peak = max(peak, rss)
        now = time.time()
        # A margin before the user's 900 GB cap; only verified descendants.
        if rss > 880_000_000_000:
            log.write(json.dumps(dict(time=now, status='RESOURCE_GUARD', rss_bytes=rss))+'\n')
            for pid in owned:
                current = stat(pid)
                if current and current['start'] == procs[pid]['start']:
                    os.kill(pid, signal.SIGTERM)
            break
        if now-last >= 30 or not roots:
            log.write(json.dumps(dict(time=now, rss_bytes=rss, peak_combined_rss_bytes=peak,
                                      processes=[procs[pid] for pid in sorted(owned)]))+'\n')
            last=now
        time.sleep(1)
