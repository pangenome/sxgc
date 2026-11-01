#!/usr/bin/env python3
"""Compare all 100 count/position sets; optionally verify a corpus prefix oracle.

The deterministic chi fixture is unrelated to the text oracle. With --rescan,
compute its answers from the same 100 oracle patterns and source bytes, instead
of incorrectly claiming its positions match the 150 MB corpus's positions.
"""
import argparse
from collections import defaultdict
from pathlib import Path
import subprocess
import json
import tempfile
p = argparse.ArgumentParser()
p.add_argument('--xsa', type=Path, required=True)
p.add_argument('--container', type=Path, required=True)
p.add_argument('--corpus', type=Path, required=True)
p.add_argument('--oracle', type=Path, default=Path('tools/binbwt/oracle.tsv'))
p.add_argument('--patterns', type=Path, default=Path('tools/binbwt/pats.fa'))
p.add_argument('--output', type=Path, required=True)
p.add_argument('--prefix-bytes', type=int)
p.add_argument('--rescan', action='store_true')
a = p.parse_args()
patterns = []
for line in a.patterns.read_bytes().splitlines():
    if line.startswith(b'>'): patterns.append(b'')
    else: patterns[-1] += line
assert len(patterns) == 100
expected = {}
for line in a.oracle.read_text().splitlines():
    if line.startswith('#'): continue
    i, count, positions = line.split('\t')
    pos = [int(v) for v in positions.split(',') if v]
    assert len(pos) == int(count)
    if a.prefix_bytes is not None:
        pos = [v for v in pos if v + len(patterns[int(i)]) <= a.prefix_bytes]
    expected[int(i)] = sorted(pos)
if a.rescan or a.prefix_bytes is not None:
    corpus = a.corpus.read_bytes()
    for i, pattern in enumerate(patterns):
        positions = []
        at = corpus.find(pattern)
        while at >= 0:
            positions.append(at)
            at = corpus.find(pattern, at + 1)
        if a.rescan: expected[i] = positions
        else: assert positions == expected[i], ('source oracle mismatch', i)
result = subprocess.run([str(a.xsa.resolve()), 'query', str(a.container), '--patterns-fasta', str(a.patterns), '-j', '16'], capture_output=True, check=True)
a.output.with_suffix('.stderr').write_bytes(result.stderr)
a.output.with_suffix('.positions.tsv').write_bytes(result.stdout)
actual = defaultdict(list)
for line in result.stdout.decode().splitlines():
    i, pos = map(int, line.split('\t'))
    actual[i].append(pos)
for i in range(100):
    assert sorted(actual[i]) == expected[i], (i, actual[i], expected[i])
# Regression: the highest absent byte can own remap code zero (virtual END).
# Check all absent bytes, both alone and embedded in otherwise present bytes.
import mmap
with a.corpus.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
    absent = [b for b in range(256) if data.find(bytes([b])) < 0]
    assert 255 in absent, 'this gate requires a corpus without 0xff'
    anchor = data[:1]
    probes = [p for b in absent if b != 0x1e
              for p in (bytes([b]), anchor + bytes([b]), bytes([b]) + anchor)]
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'absent.bin'
    path.write_bytes(b'\x1e'.join(probes) + b'\x1e')
    for output in (['--counts'], ['--output', 'jsonl'], []):
        check = subprocess.run([str(a.xsa.resolve()), 'query', str(a.container),
            '--patterns', str(path), '-j', '4', *output], capture_output=True, check=True)
        assert f'QUERY_ABSENT_SYMBOL patterns={len(probes)}'.encode() in check.stderr
        if output == ['--counts']:
            assert check.stdout.decode().splitlines() == [f'{i}\t0' for i in range(len(probes))]
        elif output:
            rows = [json.loads(line) for line in check.stdout.splitlines()]
            assert len(rows) == len(probes)
            assert all(row['count'] == 0 and row['offsets'] == [] for row in rows)
        else:
            assert check.stdout == b''
print(f'QUERY_ABSENT_SYMBOL patterns={len(probes)}; 0xff occurrences=0 (counts/jsonl/positions)')
summary = dict(status='PASS', patterns=100, counts_exact=100, positions_exact=100,
               total_positions=sum(map(len, expected.values())), rescan=a.rescan,
               prefix_bytes=a.prefix_bytes)
a.output.write_text(json.dumps(summary, indent=2)+'\n')
print(json.dumps(summary))
