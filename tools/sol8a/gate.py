#!/usr/bin/env python3
"""Radix-only producer into the unchanged band/piece/fused-finish protocol."""
import json,os,subprocess,sys,time
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');size=sys.argv[1]
root=base/f'sol8a-gate-{size}';root.mkdir(exist_ok=True)
fixture=base/'extcols-scratch'/f'mergperf-{size}-snap.txt'
remap=base/'sol6c-gate100m/remap-256.bin'
prefix=root/'frag';output=root/'product.sxi2'
env=dict(os.environ,CROSS_POOL_SLOTS='33554432',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='1100')
producer=[str(base/'sol8a-measure/radix_sweep'),'--radix',str(fixture),'--remap',str(remap),'--out-prefix',str(prefix),'--position-cap','32000000','--radix-bytes','8','--threads','16']
consumer=[str(base/'sol7e-freshstart/pile_finish'),'--prefix',str(prefix),'--corpus',str(fixture),'--remap',str(remap),'--output',str(output),'--threads','96','--poll-timeout','7200','--wit-ram-budget',str(4*1024**3),'--chi-expect',{'100m':'29716349','1b':'283296933'}[size]]
start=time.monotonic()
with (root/'producer.log').open('w') as pl,(root/'finish.log').open('w') as fl:
    p=subprocess.Popen(producer,env=env,stdout=pl,stderr=pl)
    f=subprocess.Popen(consumer,env=env,stdout=fl,stderr=fl)
    pr=p.wait();pw=time.monotonic()-start
    if pr:f.terminate()
    fr=f.wait()
bank=base/'extcols-scratch'/('xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2' if size=='100m' else 'pilev2-g2-1b/frag.sxi2')
cr=subprocess.run(['cmp',str(output),str(bank)]).returncode if pr==fr==0 else None
result=dict(size=size,producer=producer,finish=consumer,producer_rc=pr,finish_rc=fr,cmp_rc=cr,bank=str(bank),producer_wall_s=pw,total_wall_s=time.monotonic()-start,quiet_box_attested=False)
(root/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));assert pr==fr==cr==0
