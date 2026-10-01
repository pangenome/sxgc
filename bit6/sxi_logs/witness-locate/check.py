#!/usr/bin/env python3
"""One forward pass over a source text checks find-one JSONL contexts.

Usage: check.py artifact source queries.tsv first.jsonl sampled.jsonl
The sampled file comes from the existing locate path with --sample 1.
"""
import json
import struct
import sys

artifact, source, queries, first, sampled = sys.argv[1:]
patterns = dict(line.rstrip('\n').split('\t', 1) for line in open(queries)
                if line.startswith('probe'))
assert len(patterns) == 1000

with open(artifact, 'rb') as f:
    header = f.read(64)
    n = struct.unpack_from('<Q', header, 8)[0]
    reversed_storage = bool(struct.unpack_from('<Q', header, 48)[0] & 4)
    members = struct.unpack_from('<I', header, 32)[0]
    directory = f.read(40 * members)
    names = None
    for i in range(members):
        d = directory[40*i:40*(i+1)]
        if struct.unpack_from('<I', d)[0] == 6:
            off, size = struct.unpack_from('<QQ', d, 8)
            f.seek(off)
            names = f.read(size).decode().splitlines()
    starts = []
    if names:
        current = 0
        for line in names:
            name, _, length = line.split('\t')
            starts.append((name, current, int(length)))
            current += int(length) + 1
        assert current == n
    else:
        starts = [('text', 0, n)]

hits = [json.loads(line) for line in open(first)]
sample_hits = [json.loads(line) for line in open(sampled)]
seen = {h['read'] for h in hits}
sample_seen = {h['read'] for h in sample_hits}
assert len(hits) == len(seen), 'more than one hit per pattern'
assert seen == sample_seen, f'completeness mismatch: missed={sorted(sample_seen-seen)[:5]}, false={sorted(seen-sample_seen)[:5]}'

def reverse_complement(s):
    table = str.maketrans('ACGTRYSWKMBDHVNacgtryswkmbdhvn',
                         'TGCAYRSWMKVHDBNtgcayrswmkvhdbn')
    return s.translate(table)[::-1]

checks = []
for hit in hits:
    pattern = patterns[hit['read']]
    if hit.get('strand') == '-':
        pattern = reverse_complement(pattern)
    name, start, length = starts[hit['doc_id']]
    assert name == hit['name'] and hit['offset'] + len(pattern) <= length
    if reversed_storage:
        pos = start + length - hit['offset'] - len(pattern)
        expected = pattern[::-1].encode()
    else:
        pos = start + hit['offset']
        expected = pattern.encode()
    checks.append((pos, expected, hit['read']))
checks.sort()
with open(source, 'rb', buffering=1024*1024) as f:
    cursor = 0
    window_start, window = 0, b''
    for pos, pattern, read in checks:
        if pos > cursor:
            skip = pos - cursor
            while skip:
                data = f.read(min(skip, 1024*1024))
                assert data, 'source ended during forward skip'
                skip -= len(data)
            cursor = pos
        # Adjacent/overlapping checks reuse only the last 64 KiB of bytes.
        if pos < cursor:
            assert pos >= window_start
            if pos + len(pattern) > cursor:
                extra = f.read(pos + len(pattern) - cursor)
                window += extra
                cursor += len(extra)
            actual = window[pos-window_start:pos-window_start+len(pattern)]
        else:
            actual = f.read(len(pattern))
            window_start, window = pos, actual
            cursor += len(actual)
        assert actual == pattern, f'direct text mismatch: {read} at {pos}'
print(json.dumps({'patterns': 1000, 'occurring': len(seen),
                  'no_occurrence': 1000-len(seen), 'direct_text_checks': len(checks),
                  'sampled_locate_agreement': len(seen)}))
