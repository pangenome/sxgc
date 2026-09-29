#!/usr/bin/env python3
"""Measured no-clobber gate for the opt-in external front end.

Each stage journals argv, time/RSS, and polled peak allocated disk bytes. A
reference prefix compares every available front-end output byte for byte.
Inputs are either read once by PFP or copied from a retained parse; no source
text is materialized by this driver. --through-sxi requires a ready toolset.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--work',type=pathlib.Path,required=True)
p.add_argument('--scratch',type=pathlib.Path,required=True)
p.add_argument('--pfp',type=pathlib.Path,required=True)
p.add_argument('--rpfbwt',type=pathlib.Path,required=True)
p.add_argument('--input',type=pathlib.Path)
p.add_argument('--agc',action='store_true',help='decode AGC inside PFP, without a flat-text file')
p.add_argument('--parse-only',action='store_true',help='measure the text parser only; not an end-to-end gate')
p.add_argument('--retained-parse',type=pathlib.Path)
p.add_argument('--reference',type=pathlib.Path)
p.add_argument('--memory-bytes',type=int,default=3500000000)
p.add_argument('--through-sxi',action='store_true')
p.add_argument('--tools',type=pathlib.Path)
p.add_argument('--xsa',type=pathlib.Path)
p.add_argument('--terminal',default='0a')
a=p.parse_args()
if bool(a.input)==bool(a.retained_parse):p.error('choose --input or --retained-parse')
if a.parse_only and (a.retained_parse or a.through_sxi):p.error('--parse-only requires input and excludes --through-sxi')
if a.through_sxi and not(a.tools and a.xsa and a.input):p.error('--through-sxi needs --tools, --xsa, --input')
if a.through_sxi and a.agc:p.error('AGC audit is outside this text-gate driver; use the product pipeline')
for key,value in vars(a).items():
    if isinstance(value,pathlib.Path):setattr(a,key,value.resolve())
a.work.mkdir(parents=True,exist_ok=False)
a.scratch.mkdir(parents=True,exist_ok=False)
prefix=a.work/'parse'
env=dict(os.environ,SXI_SCRATCH_DIRS=str(a.scratch),OMP_NUM_THREADS='1')
missing_reference_outputs=[]

def record(**event):
    with (a.work/'journal.jsonl').open('a') as f:f.write(json.dumps(dict(time=time.time(),**event))+'\n')

def disk():
    return sum(f.stat().st_blocks*512 for root in (a.work,a.scratch) for f in root.rglob('*') if f.is_file())

def run(stage,cmd):
    cmd=list(map(str,cmd));record(stage=stage,status='START',command=cmd,
        external_settings={k:v for k,v in env.items() if k.startswith('SXI_')})
    started=time.monotonic();peak=disk()
    with (a.work/(stage+'.log')).open('w') as log:
        child=subprocess.Popen(['/usr/bin/time','-v','-o',str(a.work/(stage+'.time')),
            '/usr/bin/prlimit',f'--as={a.memory_bytes}',*cmd],stdout=log,stderr=log,env=env)
        record(stage=stage,pid=child.pid)
        while child.poll() is None:
            try:peak=max(peak,disk())
            except FileNotFoundError:pass # a scratch run may be removed between stat calls
            time.sleep(.5)
    peak=max(peak,disk()) # short stages may finish between polling samples
    rss=None
    for line in (a.work/(stage+'.time')).read_text().splitlines():
        if 'Maximum resident set size (kbytes)' in line:rss=int(line.rsplit(':',1)[1])*1024
    record(stage=stage,status='PASS' if child.returncode==0 else 'FAIL',returncode=child.returncode,
        wall_seconds=time.monotonic()-started,peak_rss_bytes=rss,peak_allocated_disk_bytes=peak,
        address_space_limit_bytes=a.memory_bytes,disk_sampling_seconds=.5)
    if child.returncode:raise RuntimeError(f'{stage}: exit {child.returncode}')

if a.retained_parse:
    for ext in ('.dict','.parse','.parse.dict','.parse.parse'):
        subprocess.run(['cp','--reflink=auto',str(a.retained_parse)+ext,str(prefix)+ext],check=True)
else:
    run('parse',[a.pfp,'-t',a.input,'-o',prefix,'-w','10','-p','100','-j','1','--tmp-dir',a.scratch]+(['--agc'] if a.agc else []))
    if a.parse_only:
        record(stage='parse-benchmark',status='PASS',end_to_end_gate=False)
        sys.exit(0)
    run('parse-l2',[a.pfp,'-i',str(prefix)+'.parse','-w','5','-p','11','-j','1','--tmp-dir',a.scratch])
if a.reference:
    for ext in ('.dict','.parse','.parse.dict','.parse.parse'):
        run('cmp-input'+ext,['cmp',str(prefix)+ext,str(a.reference)+ext])
run('frontend',[a.rpfbwt,'--l1-prefix',prefix,'--w1','10','--w2','5','--threads','1','--chunks','1','--tmp-dir',a.scratch])
if a.reference:
    for ext in ('.rlebwt','.rlebwt.meta','.ssa','.ssa_t'):
        ref=pathlib.Path(str(a.reference)+ext)
        if ref.exists():run('cmp'+ext,['cmp',str(prefix)+ext,ref])
        else:
            missing_reference_outputs.append(ext)
            record(stage='cmp'+ext,status='NOT_RUN',reason='reference absent')
if a.through_sxi:
    ri4,heads,agg,chi=(a.work/('fresh.'+ext) for ext in ('ri4','head_sa','agg','sA'))
    run('endpoints',[a.tools/'rpfbwt_endpoints',prefix,ri4,heads,a.terminal])
    run('slim',[a.tools/'slim_dump','--slim','--resolve-ri4','--dict-stream','--ri4',ri4,'--head-sa',heads,'--parse',prefix,'-t','1','-o',agg])
    run('sweep',[a.xsa,'chi-rspace','--stream-agg','--ri4',ri4,'--agg',agg,'-o',chi])
    run('audit',[a.tools/'sxi_text_audit',a.input,ri4,agg,chi,'1000'])
    run('write',[a.tools/'sxi_write','--ri4',ri4,'--heads',heads,'--chi',chi,'--output',a.work/'index.sxi','--mode','text','--orientation','forward'])
    run('validate',[a.xsa,'sxi-info',a.work/'index.sxi'])
    record(stage='chi',count=chi.stat().st_size//8,n=a.input.stat().st_size,chi_over_n=(chi.stat().st_size//8)/a.input.stat().st_size)
record(stage='gate',status='PARTIAL' if missing_reference_outputs else 'PASS',
    missing_reference_outputs=missing_reference_outputs)
