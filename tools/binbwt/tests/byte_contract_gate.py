#!/usr/bin/env python3
"""End-to-end raw-byte, absent-symbol, XML fidelity and retired-entry regressions."""
import argparse
import bz2
import json
from pathlib import Path
import random
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--xsa', type=Path, required=True)
p.add_argument('--root', type=Path, required=True)
a = p.parse_args()
xsa = str(a.xsa.resolve())
a.root.mkdir(parents=True, exist_ok=False)

def run(*args, ok=True):
    result = subprocess.run([xsa, *map(str, args)], capture_output=True)
    if ok:
        assert result.returncode == 0, result.stderr.decode(errors='replace')
    return result

start = time.monotonic()
legacy = run('build', '--text', a.root / 'does-not-exist', '--mode', 'text', ok=False)
elapsed = time.monotonic() - start
assert legacy.returncode != 0 and elapsed < 2
assert b'LEGACY_TEXT_REFUSED' in legacy.stderr and b'xsa build --tape --input' in legacy.stderr
print(legacy.stderr.decode().strip())
print(f'LEGACY_REFUSAL_SECONDS {elapsed:.6f}')
assert b'--text' not in run('build', '--help').stdout

xml = a.root / 'source.xml.bz2'
xml.write_bytes(bz2.compress(b'<mediawiki><page><title>A&amp;B</title><revision><text>x&lt;y&#255;</text></revision></page></mediawiki>'))
run('corpus', '--wikipedia', xml, '--out', a.root / 'xml')
manifest = json.loads((a.root / 'xml/MANIFEST.json').read_text())
assert manifest['entity_unescape'] == 'none'
assert (a.root / 'xml/corpus.bin').read_bytes() == b'A&amp;B\nx&lt;y&#255;\x1e'
print('XML_FIDELITY entity_unescape=none source_bytes=verbatim')

rng = random.Random(4301)
raw = bytes(rng.choice(b'abcde \n') for _ in range(1_200_000))
assert raw[-1] != 0x1e and 255 not in raw
corpus = a.root / 'raw.bin'
corpus.write_bytes(raw)
snap = run('build', '--tape', '--input', corpus, '--scratch', a.root / 'snap-refused',
           '--threads', 4, '--memory-gb', 8, '--timeout', 600, '--snap-1e', ok=False)
assert snap.returncode != 0 and b'SNAP_REFUSED' in snap.stderr
build = run('build', '--tape', '--input', corpus, '--scratch', a.root / 'index',
            '--threads', 4, '--memory-gb', 8, '--timeout', 600, '--no-chi')
(a.root / 'build.stdout').write_bytes(build.stdout)
(a.root / 'build.stderr').write_bytes(build.stderr)
assert corpus.read_bytes() == raw
journal = (a.root / 'index/xsa-build.log').read_text()
assert 'CORPUS_PRESENT_BYTES count=7 hex=0a206162636465' in journal
assert 'RAW_BYTES n=1200000 terminal=virtual-END transformation=none' in journal
patterns = [b'\xff', b'a\xff', b'\xffa', b'\x00', raw[700:724]]
path = a.root / 'patterns.bin'
path.write_bytes(b'\x1e'.join(patterns) + b'\x1e')
result = run('query', a.root / 'index/raw.sxi2', '--patterns', path, '--output', 'jsonl', '-j', 4)
(a.root / 'query.stderr').write_bytes(result.stderr)
rows = [json.loads(line) for line in result.stdout.splitlines()]
assert len(rows) == len(patterns)
for pattern, row in zip(patterns, rows):
    expected = []
    at = raw.find(pattern)
    while at >= 0:
        expected.append(at)
        at = raw.find(pattern, at + 1)
    assert row['count'] == len(expected) and sorted(row['offsets']) == expected
assert b'QUERY_ABSENT_SYMBOL patterns=4' in result.stderr
print('QUERY_ABSENT_SYMBOL patterns=4')
print('RAW_BYTE_GATE PASS 0xff occurrences=0 present_pattern_exact=1 input_bytes_unchanged=1200000')

# Zero and 0xff remain searchable when present; END's inverse byte now shifts
# to 0xfe. This prevents a special-case rejection of either boundary byte.
present_raw = raw + b'\x00\xff'
present_file = a.root / 'present.bin'
present_file.write_bytes(present_raw)
run('build', '--tape', '--input', present_file, '--scratch', a.root / 'present-index',
    '--threads', 4, '--memory-gb', 8, '--timeout', 600, '--no-chi')
path.write_bytes(b'\x00\x1e\xff\x1e\xfe\x1e')
result = run('query', a.root / 'present-index/present.sxi2', '--patterns', path,
             '--output', 'jsonl', '-j', 4)
rows = [json.loads(line) for line in result.stdout.splitlines()]
assert [row['offsets'] for row in rows] == [[len(raw)], [len(raw) + 1], []]
assert [row['count'] for row in rows] == [1, 1, 0]
assert b'QUERY_ABSENT_SYMBOL patterns=1' in result.stderr
print('PRESENT_BOUNDARY_BYTES PASS 0x00=1 0xff=1 absent_0xfe=0')
