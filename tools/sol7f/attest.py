#!/usr/bin/env python3
import json,re
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');out=Path('tools/sol7f')
records=[]
for size in ['100m','1b']:
    p8=base/f'sol7f-gate-{size}-p8-packed-final'
    p1=base/f'sol7f-gate-{size}-p1-serial-final'
    a=json.loads((p8/'result.json').read_text());s=json.loads((p1/'result.json').read_text())
    n={'100m':100000503,'1b':1000000665}[size]
    for r in [a,s]:
        assert r['merge_rc']==r['finish_rc']==r['cmp_rc']==0
        assert not r['oracle_replay']
        assert r['OVERLAP']==[0,0,0]
        for i in range(3): assert r['WALK'][i]==s['WALK'][i]+r['SEAM'][i]
    assert s['PLANNING']==s['SEAM']==[0,0,0]
    assert a['serial_whole']==s['WALK']
    log=(p8/'merge.log').read_text()
    def sums(stage):
        rows=[list(map(int,m)) for m in re.findall(r'CROSS_STAGE_WORK stage='+stage+r' pass=\d+ comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)',log)]
        return [sum(x[i] for x in rows) for i in range(3)]
    assert sums('producer')==a['stages']['heap']
    assert sums('planning_seam')==a['PLANNING']
    assert sums('overlap_seam')==a['OVERLAP']
    assert [sums('reference_range')[i]+a['stages']['prepare'][i] for i in range(3)]==s['WALK']
    stalls=[dict(producer=int(p),blocks=int(b),seconds=float(t),budget_bytes=int(q)) for p,b,t,q in re.findall(r'CROSS_SPOOL_STALL producer=(\d+) blocks=(\d+) seconds=([\d.]+) quota_bytes=(\d+)',log)]
    assert len(stalls)==8
    samples=[list(map(float,line.split())) for line in (p8/'sol7f-du.log').read_text().splitlines()]
    peak=max(int(x[3]) for x in samples)
    item=dict(size=size,n=n,P8=a,P1=s,stalls=stalls,total_blocks=sum(x['blocks'] for x in stalls),summed_blocked_thread_seconds=sum(x['seconds'] for x in stalls),allocated_disc_peak_bytes=peak,disc_peak_per_n=peak/n,output_only_peak_bytes=max(int(x[1]) for x in samples),output_apparent_peak_bytes=max(int(x[4]) for x in samples),sampler_cadence_s=10,disc_bar_pass=peak<=8.6*n,wall_bar_pass=a['merge_wall']<=320,quiet_box_attested=False)
    if size=='1b':
        assert s['WALK']==[3674629435,60485597061,1987510452]
        assert a['WALK']==[3674599453,60478673268,1987483450]
        assert a['PLANNING']==[36937,203480,2216]
        assert a['SEAM']==[-29982,-6923793,-27002]
        item['delta_vs_290_97_s']=a['merge_wall']-290.97
    records.append(item)
    print(f'SOL7F_GATE_PASS size={size} chi='+{'100m':'29716349','1b':'283296933'}[size]+' cmp=0 P1_counter_identity=1 four_counter_decomposition=exact')
(out/'measurements.json').write_text(json.dumps(records,indent=2)+'\n')
