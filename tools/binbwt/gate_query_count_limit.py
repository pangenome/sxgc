#!/usr/bin/env python3
"""Count/limit gates against the banked 150 MB oracle; optional 100 GB measurement."""
import argparse
import collections
import json
import statistics
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--xsa', required=True)
p.add_argument('--container', required=True)
p.add_argument('--large')
p.add_argument('--docs')
p.add_argument('--out', type=Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=True)
root = Path(__file__).resolve().parents[2]
patterns = [line for line in (root/'tools/binbwt/pats.fa').read_bytes().splitlines() if not line.startswith(b'>')]
oracle = {}
for row in (root/'tools/binbwt/oracle.tsv').read_text().splitlines()[1:]:
    pid, count, positions = row.split('\t')
    oracle[int(pid)] = sorted(map(int, positions.split(','))) if int(count) else []

def run(name, container, args):
    start = time.monotonic()
    r = subprocess.run([a.xsa, 'query', container, '-j', '16', *args], capture_output=True)
    wall = time.monotonic() - start
    (a.out/(name+'.tsv')).write_bytes(r.stdout)
    (a.out/(name+'.log')).write_bytes(r.stderr)
    assert r.returncode == 0, r.stderr.decode(errors='replace')
    return r.stdout, r.stderr, wall

args = ['--patterns-fasta', str(root/'tools/binbwt/pats.fa')]
full, _, full_s = run('full', a.container, args)
count, log, count_s = run('count', a.container, args+['--count'])
full_walls, count_walls = [], []
for trial in range(3):
    f, _, fs = run(f'full-repeat-{trial}', a.container, args)
    c, _, cs = run(f'count-repeat-{trial}', a.container, args+['--count'])
    assert f == full and c == count
    full_walls.append(fs)
    count_walls.append(cs)
full_s, count_s = statistics.median(full_walls), statistics.median(count_walls)
counts = {int(row.split(b'\t')[0]): int(row.split(b'\t')[-1]) for row in count.splitlines()}
assert counts == {pid: len(pos) for pid, pos in oracle.items()}
assert b'positions_located=0 lf_steps=0' in log
actual = collections.defaultdict(list)
for row in full.splitlines():
    fields = row.split(b'\t')
    actual[int(fields[0])].append(int(fields[-1]))
assert {pid: sorted(actual[pid]) for pid in oracle} == oracle
limited, _, _ = run('limited', a.container, args+['--limit', '16'])
expected = []
seen = collections.Counter()
for row in full.splitlines(keepends=True):
    pid = int(row.split(b'\t')[0])
    if seen[pid] < 16:
        expected.append(row)
    seen[pid] += 1
assert limited == b''.join(expected)
jsonl, _, _ = run('limited-jsonl', a.container, args+['--limit', '16', '--output', 'jsonl'])
for row in jsonl.splitlines():
    obj = json.loads(row)
    pid = obj['pattern_id']
    assert obj['count'] == min(16, len(oracle[pid]))
    assert obj['offsets'] == actual[pid][:16]
legacy, _, _ = run('legacy-counts', a.container, args+['--counts'])
assert {int(row.split(b'\t')[0]): int(row.split(b'\t')[1]) for row in legacy.splitlines()} == counts
all_rows, _, _ = run('all', a.container, args+['--all'])
assert all_rows == full
zero, _, _ = run('zero', a.container, args+['--limit', '0'])
assert not zero
for options in (['--count', '--limit', '16'], ['--limit'], ['--limit', '-1'], ['--limit', '1.5'], ['--limit', '18446744073709551616'], ['--count', '--output', 'jsonl'], ['--count', '--counts']):
    r = subprocess.run([a.xsa, 'query', '/nonexistent', 'the', *options], capture_output=True)
    assert r.returncode and b'query: --' in r.stderr, r.stderr
small = next(p for p in oracle if 0 < len(oracle[p]) <= 16)
plain, _, _ = run('small-full', a.container, [patterns[small]])
limited, _, _ = run('small-limited', a.container, [patterns[small], '--limit', '16'])
assert plain == limited
binary = a.out/'binary.patterns'
binary.write_bytes(b'absent\tline\nslash\\\xff\x1e')
out, _, _ = run('binary-count', a.container, ['--patterns', str(binary), '--count'])
assert out == b'0\tabsent\\tline\\nslash\\\\\xff\t0\n'
summary = [f'T1 count 100/100 exact; median warm wall={count_s:.6f}s; full locate wall={full_s:.6f}s; speedup={full_s/count_s:.3f}x',
           f'T3 low-count pattern {small}: {len(oracle[small])} hits, limit 16 byte-identical; all 100 limited outputs equal full prefixes',
           'T4 default 100/100 counts and 574/574 positions exact; --all identical; zero limit and invalid arguments passed']
if a.large:
    batch = [b'the', b'cat', b'dog', b'Wikipedia', b'encyclopedia', b'quantum', b'New York', b'United States', b'computer', b'mathematics', b'zyzzyva', b'quizzaciously', b'floccinaucinihilipilification', b'pneumonoultramicroscopicsilicovolcanoconiosis', b'antidisestablishmentarianism', b'Erwin Schrodinger', b'Godel incompleteness', b'xsa-no-such-pattern-20261102', b'QZXJQZXJQZXJ', b'0123456789abcdef0123456789abcdef']
    out, log, wall = run('large-count', a.large, ['--count', *batch])
    assert len(out.splitlines()) == 20 and b'positions_located=0 lf_steps=0' in log
    hits, _, limit_s = run('large-limit', a.large, ['the', '--limit', '16', '--docs', a.docs])
    rows = [row.split(b'\t') for row in hits.splitlines()]
    assert len(rows) == 16 and all(len(row) == 5 for row in rows)
    docs = len({row[1] for row in rows})
    summary += [f'T2 --count 100 GB: 20 patterns; wall={wall:.6f}s; positions_located=0; lf_steps=0', out.decode().rstrip(), f'T2 --limit 16 the: exactly 16 rows; distinct_docs={docs}; wall={limit_s:.6f}s']
text = '\n'.join(summary)+'\n'
(a.out/'summary.txt').write_text(text)
print(text, end='')
