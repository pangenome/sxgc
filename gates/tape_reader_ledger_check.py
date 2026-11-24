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
# THE COHORT INVARIANT, replayed: declared-but-unreleased follow-up
# metadata must be released by end of run (live==0 covers it), and the
# per-phase peaks are reported so the metadata credit is auditable.
phase_peak = collections.Counter()
phase_reserves = collections.Counter()
phase_releases = collections.Counter()
_h = collections.Counter()
with args.log.open() as f:
    for line in f:
        if line.startswith('LEDGER '):
            d = dict(re.findall(r'(\w+)=([\w-]+)', line))
            key = (int(d['unit']), d['phase'])
            amount = int(d['bytes'])
            _h[key] += amount if d['op'] == 'reserve' else -amount
            who = d['phase']
            if d['op'] == 'reserve':
                phase_reserves[who] += 1
            else:
                phase_releases[who] += 1
            phase_peak[who] = max(phase_peak[who], _h[key])
meta_key = 'follow-meta'
assert phase_reserves[meta_key] == phase_releases[meta_key], (
    'follow-meta declarations not all released:',
    phase_reserves[meta_key], phase_releases[meta_key])
if not args.partial:
    assert final and done and live == 0 and not +holding, (final, done, live)
print(json.dumps(dict(log=str(args.log), events=events, reserves=reserves,
    releases=releases, live=live, peak=peak, final=final, done=done,
    phase_peaks=dict(phase_peak),
    follow_meta_reserves=phase_reserves[meta_key],
    follow_meta_releases=phase_releases[meta_key],
    passes=passes, batches=batches, status='PARTIAL' if args.partial else 'PASS'), indent=2))
