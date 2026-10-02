#!/usr/bin/env python3
"""No-clobber v6-4 gates: repack banked v6-3 containers (format v5, codec 119) to
format v6 (codec 120, Rice-coded run permutation); exact byte-parity + size table."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time

p=argparse.ArgumentParser()
p.add_argument('--writer',required=True)
p.add_argument('--xsa',required=True)
p.add_argument('--dest',required=True)
p.add_argument('--corpus',choices=['yeast235','pile-frag','k10'],required=True)
a=p.parse_args()
here=Path(__file__).resolve().parent
v63=here.parent/'sxi2-v6-3'
source={'yeast235':Path('/mnt/nvme3n1/erikg/sxi2-v6-3-3eae0415/yeast235.sxi2'),
        'pile-frag':Path('/mnt/nvme3n1/erikg/sxi2-v6-3-main/pile-frag.sxi2'),
        'k10':Path('/mnt/nvme3n1/erikg/sxi2-v6-3-main/k10.sxi2')}[a.corpus]
dest=Path(a.dest);dest.mkdir(parents=True,exist_ok=True)
target=dest/(a.corpus+'.sxi2')
label={'yeast235':'yeast','pile-frag':'pile','k10':'k10'}[a.corpus]
mode='text' if a.corpus=='pile-frag' else 'dna'
v5=here.parent/'sxi2-v5'
reference=json.loads((v5/('pile-frag-bench-http-gate.json' if a.corpus=='pile-frag' else a.corpus+'-http-gate.json')).read_text())
old_http=v5/('pile-frag-bench-old-http.jsonl' if a.corpus=='pile-frag' else a.corpus+'-old-http.jsonl')
banked={'yeast235':{'bytes':923770376,'phi_bits_per_run':59.19,'http_median_ms':440.19117893140467},
        'pile-frag':{'bytes':2606290008,'phi_bits_per_run':42.25,'http_median_ms':0.325},
        'k10':{'bytes':20224350184,'phi_bits_per_run':71.96,'http_median_ms':4.610146046616137}}

def run(tag,cmd,output=None):
    log=here/(a.corpus+'-'+tag+'.log')
    if log.exists():raise RuntimeError('log already exists: '+str(log))
    t=time.monotonic()
    with log.open('wb') as err:
        result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=err)
    if output is not None:output.write_bytes(result.stdout)
    (here/(a.corpus+'-'+tag+'.time.json')).write_text(json.dumps({'command':[str(x) for x in cmd],'wall_seconds':time.monotonic()-t,'exit':result.returncode},indent=2)+'\n')
    if result.returncode:raise RuntimeError(f'{tag} failed ({result.returncode}); see {log}')
    return result.stdout

if target.exists():raise RuntimeError('target already exists: '+str(target))
# Never-regress fallback: on incompressible-permutation corpora the v6-4 output
# matches the v6-3 layout exactly, so the budget allows equality with the source.
run('convert',[a.writer,'--sxi',str(source),'--output',str(target),'--validator',a.xsa,'--max-bytes',str(source.stat().st_size)])
with target.open('rb') as f:header=f.read(64+40*9)
assert header[:4]==b'SXI2' and struct.unpack_from('<I',header,4)[0]==6
size=target.stat().st_size
assert size<=source.stat().st_size,'v6-4 must never exceed its v6-3 source'
r=struct.unpack_from('<Q',header,24)[0]
count=struct.unpack_from('<I',header,32)[0]
members={struct.unpack_from('<I',header,64+40*i)[0]:struct.unpack_from('<Q',header,64+40*i+16)[0] for i in range(count)}
assert members[10]==0 and members[8]>0 and 2 not in members and 3 not in members

native=here/(label+'-native-new.jsonl')
run('native',[a.xsa,'mems','--sxi',str(target),'--reads',str(v5/(a.corpus+'.reads2.fa')),
              '--min-len','50','-j','1','--mode',mode],native)
assert native.read_bytes()==(v5/(label+'-native-old.jsonl')).read_bytes(),'native MEM byte parity'
if a.corpus!='k10':
    bounded=here/(label+'-bounded-new.tsv')
    run('bounded',[a.xsa,'mems','--sxi',str(target),'--reads',str(v5/(a.corpus+'.reads2.fa')),
                   '--min-len','20','-j','1','--mode',mode,'--out','ropebwt3','-p','10'],bounded)
    old=v5/(label+'-mems-old.tsv')
    if not old.exists():old=v5/(label+'-mems2-old.tsv')
    assert bounded.read_bytes()==old.read_bytes(),'bounded MEM byte parity'
if a.corpus=='k10':startup=['--startup-timeout','2400']
else:startup=[]
# HTTP gate: cross-artifact parity vs the banked v6-3 source (old), new = v6-4.
run('http',['python3',str(here/'http_gate.py'),'--xsa',a.xsa,'--old',str(source),'--new',str(target),
     '--old-cached',str(old_http),'--pattern',reference['pattern'],'--label',a.corpus,
     '--mode',mode,'--reps',str(200 if a.corpus=='pile-frag' else 5)]+startup)
http=json.loads((here/(a.corpus+'-http-gate.json')).read_text())
assert http['byte_parity'] and http['old']['sha256']==http['new']['sha256'],'HTTP byte parity'
result={'corpus':a.corpus,'source':str(source),'target':str(target),'version':6,'r':r,
        'v63_bytes':banked[a.corpus]['bytes'],'bytes':size,
        'ratio_vs_v63':size/banked[a.corpus]['bytes'],
        'phi_bytes':members[8],'phi_bits_per_run':8*members[8]/r,
        'v63_phi_bits_per_run':banked[a.corpus]['phi_bits_per_run'],
        'native_sha256':hashlib.sha256(native.read_bytes()).hexdigest(),
        'http_sha256':http['new']['sha256'],'http_median_ms':http['new']['median_ms'],
        'v63_http_median_ms':banked[a.corpus]['http_median_ms']}
(here/(a.corpus+'-gate-summary.json')).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
