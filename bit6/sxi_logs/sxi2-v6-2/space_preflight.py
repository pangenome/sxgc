#!/usr/bin/env python3
"""Exact v4 projection: paired phi move records, LF starts derived from runs."""
import json
from pathlib import Path

here = Path(__file__).parent
rows = json.loads((here.parent / 'sxi2-v2/size-probe.json').read_text())
v5 = {x['artifact']: x for x in json.loads((here.parent / 'sxi2-v5/achieved-sizes-final.json').read_text())}

def align(x): return (x + 7) // 8 * 8
def bits(x): return (x + 7) // 8

out = []
for row in rows:
    n, r, chi = (row[k] for k in ('n', 'r', 'chi'))
    name = Path(row['path']).stem
    nw, rw = max(1, (n - 1).bit_length()), max(1, (r - 1).bit_length())
    ul = (n // r).bit_length() - 1
    cl = row['elias_fano_low_bits']
    members = row['sxi1_members']
    sizes = {
        '1': 2320 + bits(row['huffman_head_bits']) + bits(row['gamma_length_bits']),
        '4': members['4']['bytes'],
        '5': 24 + bits(chi * cl) + bits((n >> cl) + chi + 1),
    }
    for id in ('6', '7'):
        if id in members: sizes[id] = members[id]['bytes']
    sizes.update({
        '8': 40 + bits(r * ul) + bits((n >> ul) + r + 1) + bits(r * nw) + bits(r * rw),
        '9': 0,
        '10': 0,
        '11': 16 + 8 * ((r + 1023) // 1024),
    })
    total = 64 + 40 * len(sizes)
    for z in sizes.values(): total = align(total) + z
    out.append({'artifact': name, 'n': n, 'r': r, 'chi': chi,
                'members': sizes, 'projected_bytes': total,
                'v5_bytes': v5[name]['sxi2_bytes'],
                'phi_lf_bits_per_run': 8 * (sizes['8'] + sizes['10']) / r,
                'gate': 'PASSED' if total < v5[name]['sxi2_bytes'] else 'FAILED'})
print(json.dumps(out, indent=2))
if any(x['gate'] != 'PASSED' for x in out): raise SystemExit(1)
