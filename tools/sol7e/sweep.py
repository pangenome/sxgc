#!/usr/bin/env python3
"""Run the remaining gates serially after the oracle, with an honest wall label."""
import json, subprocess, time
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');oracle=base/'sol7e-gate-1b-p16-oracle/result.json'
deadline=time.monotonic()+3600
while not oracle.exists():
    assert time.monotonic()<deadline,'oracle gate timeout'
    time.sleep(1)
initial=json.loads(oracle.read_text());assert initial['merge_rc']==initial['finish_rc']==initial['cmp_rc']==0
assert not initial['STOP_30_PERCENT'],initial
assert initial['serial_whole']==[3674629435,60485597061,1987510452],initial['serial_whole']
for p in [4,8]:subprocess.run(['python3','tools/sol7e/reference_subdivide.py',str(base/'sol7e-input-1b/reference-p16.bin'),str(p),str(base/f'sol7e-input-1b/reference-p{p}.bin')],check=True)
results=[]
for p in [1,4,8,16]:
    with (base/f'sol7e-freshstart/sweep-p{p}-driver.log').open('w') as log:
        subprocess.run(['python3','tools/sol7e/gate.py','1b',str(p),'sweep'],stdout=log,stderr=subprocess.STDOUT,check=True)
    result=json.loads((base/f'sol7e-gate-1b-p{p}-sweep/result.json').read_text())
    assert not result['STOP_30_PERCENT'],result
    assert result['serial_whole']==[3674629435,60485597061,1987510452]
    if p==1:
        assert result['WALK']==result['serial_whole']
        assert result['PLANNING']==result['OVERLAP']==result['SEAM']==[0,0,0]
    else:
        baseline=results[0]['merge_wall'];speedup=baseline/result['merge_wall']
        result['speedup']=speedup
        result['amdahl_residual']=(1/speedup-1/p)/(1-1/p)
    results.append(result)
    (base/'sol7e-freshstart/sweep.json').write_text(json.dumps(dict(quiet_box_attested=False,results=results),indent=2)+'\n')
    print('SWEEP_PASS P=%d merge_wall=%.3f total_wall=%.3f cmp=0 quiet_box_attested=0'%(p,result['merge_wall'],result['total_wall']),flush=True)
