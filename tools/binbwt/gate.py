#!/usr/bin/env python3
"""bin-bwt v1 producer into the unchanged band/piece/fused-finish protocol.

usage: gate.py {100m|1b} [tag] [threads] [live_cap_positions]
Extra tags append to the scratch root so repeated runs stay fresh.
"""
import json,os,subprocess,sys,time
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');size=sys.argv[1]
tag=sys.argv[2] if len(sys.argv)>2 else ''
threads=sys.argv[3] if len(sys.argv)>3 else '96'
livecap=sys.argv[4] if len(sys.argv)>4 else '4096'
label=f'binbwt-gate-{size}'+('-'+tag if tag else '')
root=base/label;root.mkdir(exist_ok=True)
fixture=base/'extcols-scratch'/f'mergperf-{size}-snap.txt'
remap=base/'sol6c-gate100m/remap-256.bin'
prefix=root/'frag';output=root/'product.sxi2'
env=dict(os.environ,CROSS_POOL_SLOTS='33554432',CROSS_V2_PIECES='1',
         CROSS_V2_PIECE_WINDOW=os.environ.get('GATE_PIECE_WINDOW','1100'))
producer=[os.environ.get('BINBWT_BIN',str(base/'binbwt-build/binbwt')),'--binbwt',str(fixture),'--remap',str(remap),
          '--out-prefix',str(prefix),'--threads',threads,'--bin-live-cap',livecap]
consumer=[os.environ.get('XSA_FINISH_BIN',str(base/'sol7e-freshstart/xsa_finish')),'--prefix',str(prefix),'--corpus',str(fixture),
          '--remap',str(remap),'--output',str(output),'--threads','96','--poll-timeout','7200',
          '--wit-ram-budget',str(4*1024**3),
          '--chi-expect',{'100m':'29716349','1b':'283296933'}[size]]
assert not Path(str(prefix)+'.rlebwt').exists() and not Path(str(prefix)+'.pieces').exists() \
       and not output.exists(), f'gate root is not fresh (stale producer/finish output): {root}'
start=time.monotonic()
with (root/'producer.log').open('w') as pl,(root/'finish.log').open('w') as fl:
    p=subprocess.Popen(producer,env=env,stdout=pl,stderr=pl)
    f=subprocess.Popen(consumer,env=env,stdout=fl,stderr=fl)
    pr=p.wait();pw=time.monotonic()-start
    if pr:f.terminate()
    fr=f.wait()
bank=base/'extcols-scratch'/('xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2' if size=='100m' else 'pilev2-g2-1b/frag.sxi2')
cr=subprocess.run(['cmp',str(output),str(bank)]).returncode if pr==fr==0 else None
result=dict(size=size,threads=threads,live_cap=livecap,producer=producer,finish=consumer,
            producer_rc=pr,finish_rc=fr,cmp_rc=cr,bank=str(bank),producer_wall_s=pw,
            total_wall_s=time.monotonic()-start,quiet_box_attested=False,
            loadavg_before=None)
(root/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result));assert pr==fr==cr==0
