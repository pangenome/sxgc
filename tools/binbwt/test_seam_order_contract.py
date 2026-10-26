#!/usr/bin/env python3
"""Offline fixture evidence: compare finite suffix BWT with the bank's cyclic stream.

This is an explicit diagnostic, not a product algorithm. Reads the debug-only
40-byte/run dump produced with --debug-artifacts and the prebuilt finite SA.
A difference demonstrates why physically seed-first ordering changes m1 bytes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct

p = argparse.ArgumentParser(description=__doc__)
for key in ('corpus', 'remap', 'sa', 'cyclic-stream', 'baseline'):
    p.add_argument('--' + key, required=True)
a = p.parse_args()
t = Path(a.corpus).read_bytes()
m = Path(a.remap).read_bytes()
assert len(m) == 256 and t
t = bytes(m[c] for c in t)
n = len(t)
raw = Path(a.sa).read_bytes()
assert len(raw) == 48 + 8*n
sa = [v for v, in struct.iter_unpack('<Q', raw[48:])]
assert sorted(sa) == list(range(n))
finite = bytes(t[(pos-1) % n] for pos in sa)
records = list(struct.iter_unpack('<QQQQIB3x', Path(a.cyclic_stream).read_bytes()))
cyclic = b''.join(bytes([c])*length for idx, rk, h, tail, length, c in records)
assert len(cyclic) == n
mismatches = [i for i in range(n) if finite[i] != cyclic[i]]
assert mismatches, 'This fixture does not distinguish the two orders'
print(json.dumps(dict(n=n, baseline_sha256=hashlib.sha256(Path(a.baseline).read_bytes()).hexdigest(),
    cyclic_runs=len(records), finite_runs=1+sum(x != y for x,y in zip(finite, finite[1:])),
    bwt_mismatches=len(mismatches), first_mismatches=mismatches[:32],
    finite_bwt_sha256=hashlib.sha256(finite).hexdigest(),
    cyclic_bwt_sha256=hashlib.sha256(cyclic).hexdigest()), indent=2))
