#!/usr/bin/env python3
"""Replay every reservation/release and require an empty terminal ledger."""
import argparse
import collections
import hashlib
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser()
parser.add_argument('log', type=Path)
parser.add_argument('--partial', action='store_true')
args = parser.parse_args()
live = peak = events = reserves = releases = 0
holding = collections.Counter()
final = done = None
passes = []
batches = []
with args.log.open() as f:
    for line in f:
        if line.startswith('LEDGER '):
            d = dict(re.findall(r'(\w+)=([\w-]+)', line))
            events += 1
            assert int(d['seq']) == events, (events, line)
            key = (int(d['unit']), d['phase'])
            amount = int(d['bytes'])
            if d['op'] == 'reserve':
                reserves += 1
                holding[key] += amount
                live += amount
            else:
                assert d['op'] == 'release'
                releases += 1
                assert holding[key] >= amount
                holding[key] -= amount
                live -= amount
            assert live == int(d['live']) and 0 <= live <= int(d['cap']), line
            peak = max(peak, live)
        elif line.startswith('LEDGER_FINAL '):
            final = line.strip()
        elif line.startswith('SS_DONE '):
            done = line.strip()
        elif line.startswith('PASS '):
            passes.append(line.strip())
        elif line.startswith('TAPE_BATCH '):
            batches.append(line.strip())
if not args.partial:
    assert final and done and live == 0 and not +holding, (final, done, live)
print(json.dumps(dict(log=str(args.log), events=events, reserves=reserves,
    releases=releases, live=live, peak=peak, final=final, done=done,
    passes=passes, batches=batches, status='PARTIAL' if args.partial else 'PASS'), indent=2))
