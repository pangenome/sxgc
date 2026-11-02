#!/usr/bin/env python3
"""S1/S3/S4: positional/file parity, document discovery, and independent byte oracle."""
import argparse
import collections
import json
import pathlib
import subprocess
import sys

from gate_query_docs import prepare, check

p = argparse.ArgumentParser()
p.add_argument('--xsa', type=pathlib.Path, required=True)
p.add_argument('--container', type=pathlib.Path, required=True)
p.add_argument('--corpus', type=pathlib.Path, required=True)
p.add_argument('--out', type=pathlib.Path, required=True)
a = p.parse_args()
a.out.mkdir(parents=True, exist_ok=False)
xsa = str(a.xsa.resolve())
plain = a.out/'plain'
plain.mkdir()
container = plain/'c.sxi2'
container.symlink_to(a.container.resolve())
data = a.corpus.read_bytes()
patterns = [b'foo', b'bar', b'y amazing recipes using ', b'es, as defensive structu', b'>literal\\n', b'\xff', b'--literal']
patfile = a.out/'patterns.bin'
patfile.write_bytes(b'\x1e'.join(patterns)+b'\x1e')

def run(name, args, c=container):
    r = subprocess.run([xsa, 'query', str(c), *args], capture_output=True)
    (a.out/(name+'.tsv')).write_bytes(r.stdout)
    (a.out/(name+'.log')).write_bytes(r.stderr)
    assert r.returncode == 0, r.stderr.decode(errors='replace')
    return r.stdout, r.stderr

pos, log = run('positional', ['-j', '16', '--all', '--', *patterns])
assert b'QUERY_DOCS source=none' in log
actual = sorted(tuple(map(int, row.split(b'\t'))) for row in pos.splitlines())
expected = []
for pid, pattern in enumerate(patterns):
    offset = data.find(pattern)
    while offset >= 0:
        expected.append((pid, offset))
        offset = data.find(pattern, offset+1)
assert actual == expected, 'independent corpus oracle mismatch'
file, _ = run('file', ['--patterns', str(patfile), '-j', '16'])
assert file == pos
print(f'S1 PASS positional/file exact; independent oracle hits={len(actual)}', flush=True)
conflict = subprocess.run([xsa, 'query', str(container), 'foo', '--patterns', str(patfile)], capture_output=True)
assert conflict.returncode and b'ambiguous pattern id numbering' in conflict.stderr
for bad in ('',):
    r = subprocess.run([xsa, 'query', str(container), bad], capture_output=True)
    assert r.returncode and b'1..=65536' in r.stderr

docs = a.out/'corpus with spaces'
prepare(a.corpus, docs)
recorded = a.out/'recorded'
recorded.mkdir()
rc = recorded/'c.sxi2'
rc.symlink_to(a.container.resolve())
m = json.loads((docs/'MANIFEST.json').read_text())
(recorded/'xsa-build.log').write_text(f'CONTAINER_CORPUS_SIDECARS dir={json.dumps(str(docs.resolve()))} seps=seps.bin titles=titles.bin docs={m["doc_count"]} corpus_bytes={m["corpus_bytes"]}\n')
rows, log = run('recorded', ['--patterns', str(patfile), '-j', '16'], rc)
assert b'QUERY_DOCS source=recorded' in log
check(docs, a.out/'recorded.tsv', a.out/'positional.tsv')
# A different corpus must never supply titles, even when explicitly requested.
wrong = a.out/'different-corpus'
tiny = a.out/'tiny.bin'
tiny.write_bytes(b'wrong title\nfoo\x1e')
prepare(tiny, wrong)
rows, log = run('mismatch', ['foo', 'bar', '--docs', str(wrong), '-j', '16'])
assert b'corpus metadata mismatch' in log and b'QUERY_DOCS source=none' in log
assert sorted(tuple(map(int, r.split(b'\t'))) for r in rows.splitlines()) == [r for r in expected if r[0] < 2]
rows, log = run('fallback', ['--patterns', str(patfile), '--docs', str(wrong), '-j', '16'], rc)
assert b'corpus metadata mismatch' in log and b'QUERY_DOCS source=recorded' in log
assert rows == (a.out/'recorded.tsv').read_bytes()
# Sibling takes precedence; explicit takes precedence over sibling.
for name in ('seps.bin', 'titles.bin'):
    (recorded/name).symlink_to((docs/name).resolve())
_, log = run('sibling', ['foo', '-j', '16'], rc)
assert b'QUERY_DOCS source=sibling' in log
_, log = run('explicit', ['foo', '--docs', str(docs), '-j', '16'], rc)
assert b'QUERY_DOCS source=docs' in log
rows, _ = run('jsonl', ['foo', 'bar', '--output', 'jsonl', '-j', '16'], rc)
parsed = [json.loads(line) for line in rows.splitlines()]
assert [(r['pattern_id'], r['count']) for r in parsed] == [(i, sum(p == i for p, _ in expected)) for i in range(2)]
assert sorted((r['pattern_id'], pos) for r in parsed for pos in r['offsets']) == [r for r in expected if r[0] < 2]
print('S3 PASS different-corpus warning, plain degradation and recorded fallback; precedence verified', flush=True)
print('S4 PASS plain id/offset output; JSONL machine format unchanged', flush=True)
