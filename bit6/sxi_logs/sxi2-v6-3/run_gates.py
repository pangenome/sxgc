#!/usr/bin/env python3
"""No-clobber v5-input SXI2 v5 association and exact byte-parity gate."""
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
v5=here.parent/'sxi2-v5'
source=Path('/mnt/nvme3n1/erikg/sxi2-v5')/(a.corpus+'.sxi2')
dest=Path(a.dest);dest.mkdir(parents=True,exist_ok=True)
target=dest/(a.corpus+'.sxi2')
label={'yeast235':'yeast','pile-frag':'pile','k10':'k10'}[a.corpus]
mode='text' if a.corpus=='pile-frag' else 'dna'
reference=json.loads((v5/('pile-frag-bench-http-gate.json' if a.corpus=='pile-frag' else a.corpus+'-http-gate.json')).read_text())
old_http=v5/('pile-frag-bench-old-http.jsonl' if a.corpus=='pile-frag' else a.corpus+'-old-http.jsonl')

def run(tag,cmd,output=None):
    log=here/(a.corpus+'-'+tag+'.log')
    if log.exists():raise RuntimeError('log already exists: '+str(log))
    t=time.monotonic()
    with log.open('wb') as err:
        result=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=err)
    if output is not None:output.write_bytes(result.stdout)
    (here/(a.corpus+'-'+tag+'.time.json')).write_text(json.dumps({'command':cmd,'wall_seconds':time.monotonic()-t,'exit':result.returncode},indent=2)+'\n')
    if result.returncode:raise RuntimeError(f'{tag} failed ({result.returncode}); see {log}')
    return result.stdout

if target.exists():raise RuntimeError('target already exists: '+str(target))
run('convert',[a.writer,'--sxi',str(source),'--output',str(target),'--validator',a.xsa,'--max-bytes',str(source.stat().st_size-1)])
with target.open('rb') as f:header=f.read(64+40*9)
assert header[:4]==b'SXI2' and struct.unpack_from('<I',header,4)[0]==5
size=target.stat().st_size
assert size<source.stat().st_size
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
cmd=['python3',str(here/'http_gate.py'),'--xsa',a.xsa,'--old',str(source),'--new',str(target),
     '--old-cached',str(old_http),'--pattern',reference['pattern'],'--label',a.corpus,
     '--mode',mode,'--reps',str(200 if a.corpus=='pile-frag' else 5)]
if a.corpus=='k10':cmd+=['--startup-timeout','2400']
run('http',cmd)
http=json.loads((here/(a.corpus+'-http-gate.json')).read_text())
assert http['byte_parity'] and http['old']['sha256']==http['new']['sha256'],'HTTP byte parity'
result={'corpus':a.corpus,'source':str(source),'target':str(target),'version':5,'r':r,
        'source_bytes':source.stat().st_size,'bytes':size,'phi_bytes':members[8],
        'phi_bits_per_run':8*members[8]/r,'native_sha256':hashlib.sha256(native.read_bytes()).hexdigest(),
        'http_sha256':http['new']['sha256'],'http_median_ms':http['new']['median_ms'],
        'v6_2_median_ms':{'yeast235':382.3,'pile-frag':0.362,'k10':3.474}[a.corpus]}
(here/(a.corpus+'-gate-summary.json')).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
