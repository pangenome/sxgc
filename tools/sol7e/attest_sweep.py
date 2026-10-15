#!/usr/bin/env python3
"""Check the final large-gate journals without running another walk."""
import json,re,sys
from pathlib import Path
root=Path('/mnt/nvme2n1/erikg');kind=sys.argv[1] if len(sys.argv)>1 else 'padded'
summary=json.loads((root/f'sol7e-freshstart/sweep-{kind}.json').read_text())
assert [r['P'] for r in summary['results']]==[1,4,8,16]
expected=[3674629435,60485597061,1987510452]
def plus(rows):return [sum(row[i] for row in rows) for i in range(3)]
for r in summary['results']:
    p=r['P'];directory=root/f'sol7e-gate-1b-p{p}-{kind}'
    log=(directory/'merge.log').read_text();finish=(directory/'finish.log').read_text()
    groups={}
    for stage,index,a,b,c in re.findall(r'CROSS_STAGE_WORK stage=(\w+) pass=(\d+) comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)',log):
        groups.setdefault(stage,{})[int(index)]=[int(a),int(b),int(c)]
    assert r['merge_rc']==r['finish_rc']==r['cmp_rc']==0
    assert 'PILE_FINISH_DONE chi=283296933' in finish
    assert groups['WALK'][0]==plus([groups['heap'][0],groups['prepare'][0]])==r['WALK']
    assert groups['PLANNING'][0]==groups['plan'][0]==r['PLANNING']
    assert groups['OVERLAP'][0]==r['OVERLAP']==[0,0,0]
    assert r['serial_whole']==expected
    if p>1:
        for stage in ['producer','planning_seam','overlap_seam','reference_range']:assert len(groups[stage])==p
        assert plus(groups['producer'].values())==groups['heap'][0]
        assert plus(groups['planning_seam'].values())==r['PLANNING']
        assert plus([plus(groups['reference_range'].values()),groups['prepare'][0]])==expected
        signed=[list(map(int,m[1:])) for m in re.findall(r'CROSS_SEAM_WORK seam=(\d+) comparisons=(-?\d+) symbols_compared=(-?\d+) probes=(-?\d+) signed=1',log)]
        assert len(signed)==p and plus(signed)==r['SEAM']
        assert [r['WALK'][i]-expected[i] for i in range(3)]==r['SEAM']
        assert 'oracle_reference' not in groups
        assert 'replay=0' in log
    else:
        assert r['WALK']==expected and r['PLANNING']==r['SEAM']==[0,0,0]
    assert not r['STOP_30_PERCENT']
    print(f"PWAY_LEDGER_PASS P={p} WALK={r['WALK']} PLANNING={r['PLANNING']} OVERLAP={r['OVERLAP']} SEAM={r['SEAM']} chi=283296933 cmp=0 replay=0")
Path('tools/sol7e/sweep.json').write_text(json.dumps(summary,indent=2)+'\n')
print('PWAY_SWEEP_ATTESTED cases=4 P=1,4,8,16 per_producer_sums=exact baseline=exact no_replay=1 overhead_under_30pct=1 quiet_box=UNVALIDATED')
