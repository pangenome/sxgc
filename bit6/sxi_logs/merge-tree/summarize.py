#!/usr/bin/env python3
"""Write measured serial/tree and explicitly linear 50 GB projection table."""
import json
import pathlib
import re

J = pathlib.Path(__file__).resolve().parent
serial = json.loads((J.parent / 'chunk-merge-v3/measurements.json').read_text())
log = (J / 'tree.log').read_text()
levels = [(int(a), int(b), float(c)) for a, b, c in re.findall(
    r'^TREE_LEVEL level=(\d+) pairs=(\d+) wall_seconds=([\d.]+)$', log, re.M)]
assert [a for a, _, _ in levels] == list(range(4)), levels
passed = re.search(r'^TREE_PASS chunks=16 levels=4 n=1082130213 wall_seconds=([\d.]+)$',
                   log, re.M)
assert passed
assert sorted((J / 'four-file-gate.log').read_text().splitlines()) == sorted(
    f'CMP_PASS {ext}' for ext in ('.rlebwt', '.rlebwt.meta', '.ssa', '.ssa_t'))
assert 'CHI_PASS count=306164765 bytes=2449318120' in (J / 'chi-gate.log').read_text()
fragment_n = 1082130213
pile_n = 50_000_000_000
serial_wall = serial['merge_wall_seconds']
tree_wall = float(passed[1])
lines = [
    '# Pairwise BCR tree results', '',
    '| Fragment route | Wall (s) | Relative to serial |',
    '| --- | ---: | ---: |',
    f'| Serial merge, 16 chunks | {serial_wall:.2f} | 1.00× |',
    f'| Tree merge, 16 chunks | {tree_wall:.2f} | {serial_wall/tree_wall:.2f}× |',
    '',
    '| Fragment level | Parallel pairs | Measured wall (s) |',
    '| ---: | ---: | ---: |',
]
lines.extend(f'| {level+1} | {pairs} | {wall:.2f} |' for level, pairs, wall in levels)
lines.extend(['', '## 50 GB / 26 chunk projection', '',
    'The estimates below scale measured fragment level walls linearly by the '
    'ratio of bytes in the largest pair. They are scheduling estimates, not '
    'measurements; BCR memory and time can grow nonlinearly with run count.', '',
    '| Level | Parallel pairs | Largest pair (GB) | Projected wall (s) |',
    '| ---: | ---: | ---: | ---: |'])
projection=[]
for level in range(5):
    pairs=(13, 6, 3, 2, 1)[level]
    largest=min(2**(level+1),26)*pile_n/26
    base_level=min(level,3)
    base_largest=2**(base_level+1)*fragment_n/16
    projected=levels[base_level][2]*largest/base_largest
    projection.append(projected)
    lines.append(f'| {level+1} | {pairs} | {largest/1e9:.2f} | {projected:.0f} |')
lines.extend(['', f'Projected fully parallel level sum: **{sum(projection):.0f} s**; '
    f'linear serial projection: **{serial_wall*pile_n/fragment_n:.0f} s**.',
    'Serial fragment peak RSS was 7.14 GB. Linear memory scaling to 50 GB '
    'would be about 330 GB for the final BCR state, above the 64 GB budget. '
    'Even a 15.38 GB level-3 pair projects to about 102 GB. The 50 GB run '
    'therefore needs a memory bounded BCR state before it can be attempted; '
    'the wall projection is conditional on that change.', ''])
(J / 'TABLE.md').write_text('\n'.join(lines))
print(f'TREE_SUMMARY_PASS levels=4 wall={tree_wall:.2f} chi=306164765')
