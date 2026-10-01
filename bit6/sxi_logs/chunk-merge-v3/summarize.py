#!/usr/bin/env python3
"""Write the measured fragment table after all batch and gate logs exist."""
import json
import pathlib
import re

JOURNAL = pathlib.Path(__file__).resolve().parent
sort_log = (JOURNAL / 'chunk-sort.log').read_text()
merge_log = (JOURNAL / 'merge.log').read_text()
gate_log = (JOURNAL / 'four-file-gate.log').read_text()
chi_log = (JOURNAL / 'chi-gate.log').read_text()

sort_rows = {}
for m in re.finditer(r'^CHUNK index=(\d+) offset=(\d+) n=(\d+) runs=(\d+) '
                     r'sort_wall_seconds=([\d.]+)', sort_log, re.M):
    index, offset, n, runs, wall = m.groups()
    sort_rows[int(index)] = dict(offset=int(offset), n=int(n),
                                 local_runs=int(runs), sort_wall_seconds=float(wall))
insert_rows = {}
for m in re.finditer(r'^BATCH index=(\d+) offset=(\d+) n=(\d+) runs=(\d+) '
                     r'insert_wall_seconds=([\d.]+) merged_n=(\d+)', merge_log, re.M):
    index, offset, n, runs, wall, merged_n = m.groups()
    insert_rows[int(index)] = dict(offset=int(offset), n=int(n),
                                   local_runs=int(runs), insert_wall_seconds=float(wall),
                                   padded_merged_n=int(merged_n))
assert set(sort_rows) == set(insert_rows) == set(range(16))
for i in range(16):
    assert all(sort_rows[i][key] == insert_rows[i][key]
               for key in ('offset', 'n', 'local_runs'))
merge = re.search(r'^MERGE_PASS chunks=16 merged_n=1082130213 padded_n=1082130223 '
                  r'runs=(\d+) merge_wall_seconds=([\d.]+)$', merge_log, re.M)
assert merge and int(merge[1]) == 397723016
assert sorted(gate_log.splitlines()) == sorted(f'CMP_PASS {ext}' for ext in
                                              ('.rlebwt', '.rlebwt.meta', '.ssa', '.ssa_t'))
assert 'CHI_PASS count=306164765 bytes=2449318120' in chi_log

def elapsed(path):
    value = re.search(r'Elapsed \(wall clock\) time \(h:mm:ss or m:ss\): ([\d:.]+)',
                      path.read_text())
    assert value, path
    parts = [float(x) for x in value[1].split(':')]
    seconds = 0.0
    for part in parts:
        seconds = 60 * seconds + part
    return seconds

frontend_wall = elapsed(JOURNAL / 'chunk-sort.time')
merge_wall = elapsed(JOURNAL / 'merge.time')
rows = [dict(index=i, **sort_rows[i], insert_wall_seconds=insert_rows[i]['insert_wall_seconds'])
        for i in range(16)]
data = dict(chunks=rows, chunk_frontend_wall_seconds=frontend_wall,
            merge_wall_seconds=merge_wall, merge_reported_wall_seconds=float(merge[2]),
            chunk_merge_total_wall_seconds=frontend_wall + merge_wall,
            monolithic_banked_six_stage_wall_seconds=4789.07,
            reference=dict(n=1082130213, normalized_runs=397723010, chi=306164765),
            gate='PASS: four raw files byte identical; chi from merged ri4 and PFP parse')
(JOURNAL / 'measurements.json').write_text(json.dumps(data, indent=2) + '\n')
table = ['| Chunk | Sort wall (s) | Batch BCR insert wall (s) |',
         '| ---: | ---: | ---: |']
table.extend(f'| {row["index"]} | {row["sort_wall_seconds"]:.3f} | '
             f'{row["insert_wall_seconds"]:.3f} |' for row in rows)
table.extend(['', 'BCR prepends symbols. Batches were inserted in index order 15→0 '
              'so the final text is chunk 0 through chunk 15.',
              f'Chunk front end total: **{frontend_wall:.2f} s**.',
              f'Merge total including output and SA samples: **{merge_wall:.2f} s**.',
              f'Chunk route total: **{frontend_wall + merge_wall:.2f} s**.',
              'Monolithic PFP six-stage banked total: **4789.07 s**.',
              '', 'The route is a correctness gate, not a speed result. At this fragment scale, '
              + ('monolithic PFP is faster.' if frontend_wall + merge_wall > 4789.07
                 else 'the measured chunk route is faster.'),
              'The value of chunk construction is a bounded-memory path when a full PFP '
              'dictionary cannot be held; an O(runs) cross-LCP merge remains open.', ''])
(JOURNAL / 'TABLE.md').write_text('\n'.join(table))
print('SUMMARY_PASS chunks=16 four_files=identical chi=306164765')
