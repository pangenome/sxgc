#!/usr/bin/env python3
"""Cross-check fresh-start ranges against the sol7d seeded oracle.

The diagnostic replay is always visible as oracle_reference and never counted
as production PLANNING. Signed SEAM deltas are attribution, not extra work to
add to already charged producer sums. OVERLAP counts actual repeated rows (0).
"""
import json, os, re, subprocess, sys
from pathlib import Path
binary = str(Path(sys.argv[1]).resolve())
root = Path(sys.argv[2]); root.mkdir(parents=True, exist_ok=True)
def run(kind):
    env = dict(os.environ)
    env.pop('CROSS_SEED_HEAP', None)
    env.pop('CROSS_SEAM_ORACLE', None)
    env['CROSS_SEED_HEAP' if kind == 'seeded' else 'CROSS_SEAM_ORACLE'] = '1'
    path = root / (kind + '.log')
    with path.open('w') as out:
        subprocess.run([binary], env=env, stdout=out, stderr=subprocess.STDOUT, check=True)
    cases = []
    for line in path.read_text().splitlines():
        m = re.fullmatch(r'SEAM_CASE fixture=(\d+) P=(\d+) mode=(\w+)', line)
        if m:
            c = dict(fixture=int(m[1]), P=int(m[2]), mode=m[3], stages={}, producers={}, planning={}, overlap={}, reference={}, seam={})
            cases.append(c)
        m = re.fullmatch(r'CROSS_STAGE_WORK stage=(\w+) pass=(\d+) comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)', line)
        if m:
            name, index = m[1], int(m[2]); values = list(map(int, m.groups()[2:]))
            group = {'producer':'producers', 'planning_seam':'planning', 'overlap_seam':'overlap', 'reference_range':'reference'}.get(name)
            if group: c[group][index] = values
            else: c['stages'][name] = values
        m = re.fullmatch(r'CROSS_SEAM_WORK seam=(\d+) comparisons=(-?\d+) symbols_compared=(-?\d+) probes=(-?\d+) signed=1', line)
        if m: c['seam'][int(m[1])] = list(map(int, m.groups()[1:]))
        m = re.fullmatch(r'SEAM_TOTAL comparisons=(\d+) symbols=(\d+) probes=(\d+)', line)
        if m: c['total'] = list(map(int, m.groups()))
    assert len(cases) == 40
    assert 'SHARD_ROW_SEAM_WITNESS_PASS cases=40 seams=242' in path.read_text()
    return cases, path
# Parse stages without assuming the original trace identity for fresh heaps.
seeded, seedlog = run('seeded')
fresh, freshlog = run('fresh')
def summed(values): return [sum(v[i] for v in values) for i in range(3)]
base = {(c['fixture'], c['mode']): c for c in fresh if c['P'] == 1}
for f, s in zip(fresh, seeded):
    assert (f['fixture'], f['P'], f['mode']) == (s['fixture'], s['P'], s['mode'])
    b = base[f['fixture'], f['mode']]
    assert f['stages']['prepare'] == b['stages']['prepare']
    assert len(f['producers']) == len(f['reference']) == f['P']
    assert summed(f['reference'].values()) == b['stages']['heap']
    assert f['reference'] == s['producers'] # seeded exact trajectory, per range
    assert summed(s['producers'].values()) == b['stages']['heap']
    assert summed(f['planning'].values()) == f['stages']['plan']
    assert summed(f['overlap'].values()) == [0,0,0]
    assert summed(f['producers'].values()) == f['stages']['heap']
    for index, actual in f['producers'].items():
        assert f['seam'][index] == [actual[i] - f['reference'][index][i] for i in range(3)]
        print(f"SEAM_HALO_PASS fixture={f['fixture']} P={f['P']} mode={f['mode']} seam={index} PLANNING={f['planning'][index]} OVERLAP={f['overlap'][index]} SEAM={f['seam'][index]} rows=exact")
    f['WALK_HEAP'] = f['stages']['heap']
    f['WALK'] = summed([f['stages']['prepare'], f['WALK_HEAP']])
    f['PLANNING'] = f['stages']['plan']
    f['OVERLAP'] = [0,0,0]
    f['SEAM'] = summed(f['seam'].values())
    assert f['total'] == summed([f['WALK'], f['PLANNING']])
    if f['P'] == 1: assert f['PLANNING'] == f['OVERLAP'] == f['SEAM'] == [0,0,0]
(root/'counter-witness.json').write_text(json.dumps(dict(result='PASS', cases=fresh, oracle_log=str(seedlog), fresh_log=str(freshlog), production_bands_validated=False), indent=2)+'\n')
print('FRESH_SEAM_COUNTER_WITNESS_PASS cases=40 fixtures=5 P=1,4,8,16 modes=cyclic,dollar rows=exact seeded_reference=exact per_seam_journal=exact OVERLAP=0 P1_extra=0')
