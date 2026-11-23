"""Attribute cumulative /proc/self/io deltas up to a checkpoint or last trace.

An @boundary record closes the *previous* phase. No inference from stage names
of periodic samples. Bytes are process-attributed reads, not device IO totals.
"""
import argparse
from collections import Counter
from pathlib import Path
import re


def costs(path, until):
    prev, phase, found = 0, 'setup', False
    total = Counter()
    for line in path.read_text().splitlines():
        m = re.match(r'(TRACE|CHECKPOINT|PROBE) .*?reads=(\d+)', line)
        if not m:
            continue
        val = int(m[2]); assert val >= prev
        total[phase] += val-prev; prev = val
        if 'stage=@' in line:
            phase = re.search(r'stage=@(\S+)', line)[1]
        if until and re.match(rf'(CHECKPOINT|PROBE) rows={until} ', line):
            found = True; break
    if until and not found:
        raise ValueError(f'{path}: no row {until} checkpoint')
    assert sum(total.values()) == prev
    return prev, dict(total)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('logs', nargs='+', type=Path)
    p.add_argument('--rows', type=int, default=0)
    a = p.parse_args()
    for log in a.logs:
        end, phases = costs(log, a.rows)
        print(log, end, phases)
