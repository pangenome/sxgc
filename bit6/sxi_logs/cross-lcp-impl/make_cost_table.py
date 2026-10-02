#!/usr/bin/env python3
"""Build COST_TABLE.md from the journaled CROSS_PAIR rows and the banked
batch-BCR walls. Run from anywhere; reads the journal directory."""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
J = HERE.parent

BANKED_SERIAL = 20599.00   # chunk-merge-v3 TABLE.md / merge.log
BANKED_TREE = 19596.80     # chunk-merge-v3 tree merge.log TREE_PASS


def pairs(logpath):
    rows = []
    for line in logpath.read_text().splitlines():
        if not line.startswith('CROSS_PAIR'):
            continue
        d = dict(re.findall(r'(\w+)=([^ ]+)', line))
        rows.append(d)
    return rows


def total_wall(rows):
    return sum(float(r['wall_total']) for r in rows)


def fmt(rows, name, banked):
    out = []
    out.append(f'## {name}\n')
    out.append('| # | n | runs(out) | wall(s) | comparisons | symbols compared | probes | max LCE | anchors A/B |')
    out.append('|---:|---:|---:|---:|---:|---:|---:|---:|---:|')
    for i, r in enumerate(rows):
        out.append(
            f"| {i} | {int(r['nA']) + int(r['nB'])} | {r['out_runs']} | {float(r['wall_total']):.1f} "
            f"| {r['comparisons']} | {r['symbols_compared']} | {r['probes']} | {r['max_lce']} "
            f"| {r['anchors_A']}/{r['anchors_B']} |")
    tw = total_wall(rows)
    sym = sum(int(r['symbols_compared']) for r in rows)
    cmps = sum(int(r['comparisons']) for r in rows)
    probes = sum(int(r['probes']) for r in rows)
    speed = banked / tw if tw else float('inf')
    out.append('')
    out.append(f'- pairs: {len(rows)}')
    out.append(f'- total merge wall (sum of pairwise walls): **{tw:.1f} s**')
    out.append(f'- banked batch-BCR wall on the same 16 chunks: **{banked:.2f} s**')
    out.append(f'- speedup: **{speed:.1f}x**')
    out.append(f'- total comparisons: {cmps} ({cmps / total_n(rows):.2f} per text symbol)')
    out.append(f'- total symbols compared (journaled verification work): {sym} '
               f'({sym / total_n(rows):.2f} per text symbol)')
    out.append(f'- total hash probes: {probes}')
    out.append('')
    return '\n'.join(out)


def total_n(rows):
    return sum(int(r['nA']) + int(r['nB']) for r in rows)


def main():
    tree = pairs(J / 'merge-tree.log')
    serial = pairs(J / 'merge-serial.log')
    w = []
    w.append('# Cross-LCP pairwise merge: cost table\n')
    w.append('All rows come from the journaled CROSS_PAIR lines of the exact merge '
             'runs that passed the four-file byte-identity gate. Every comparison '
             'is exact: hash proposals are always fully verified against the text '
             '(SLIM_FP discipline), so `symbols compared` counts real symbol reads. '
             'The cost is O(positions) comparisons with O(1) amortized verified '
             'work each - no per-character dynamic-BWT insertion anywhere.\n')
    if tree:
        w.append(fmt(tree, 'Balanced pairwise tree (16 chunks, 15 merges)', BANKED_TREE))
    if serial:
        w.append(fmt(serial, 'Serial left fold (16 chunks, 15 merges)', BANKED_SERIAL))
    w.append('## Reference banked batch-BCR walls\n')
    w.append(f'- serial: {BANKED_SERIAL} s (chunk-merge-v3 merge.log MERGE_PASS)')
    w.append(f'- tree: {BANKED_TREE} s (chunk-merge-v3 TREE_PASS)')
    w.append('')
    print('\n'.join(w))


if __name__ == '__main__':
    main()
