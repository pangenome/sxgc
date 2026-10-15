#!/usr/bin/env python3
"""Reusable quiet-box gate driver; preserves input chunks for the P sweep.

CROSS_SEAM_ORACLE diagnostic replay is explicitly timed and reported, never
presented as a launch wall. No subtraction of its wall is a benchmark.
"""
import json, os, re, resource, subprocess, sys, time
from pathlib import Path
base=Path('/mnt/nvme2n1/erikg');dev=base/'sol7e-freshstart'
size=sys.argv[1];p=int(sys.argv[2]);kind=sys.argv[3] if len(sys.argv)>3 else 'oracle'
inputs=base/('sol7e-input-'+size);root=base/f'sol7f-gate-{size}-p{p}-{kind}'
root.mkdir(exist_ok=True);inputs.mkdir(exist_ok=True)
fixture=base/'extcols-scratch'/f'mergperf-{size}-snap.txt';n=fixture.stat().st_size
remap=inputs/'remap-256.bin'
if not remap.exists(): remap.write_bytes((base/'sol6c-gate100m/remap-256.bin').read_bytes())
env=dict(os.environ,CHUNK_WORKERS='16',CHUNK_POS_MODE='compact',CROSS_POS_MODE='compact',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='1100',CROSS_WM_PACK='1',CROSS_POOL_SLOTS='33554432',CROSS_V2_PRODUCERS=str(p))
env.pop('CROSS_DELETE_CONSUMED',None);env.pop('CROSS_SEED_HEAP',None)
env.pop('CROSS_SEAM_ORACLE',None);env.pop('CROSS_SEAM_REFERENCE',None)
if p>1:
    env['CROSS_SEAM_REFERENCE']=str(inputs/f'reference-p{p}.bin')
    if kind=='oracle':env['CROSS_SEAM_ORACLE']='1'
def cap():resource.setrlimit(resource.RLIMIT_AS,(100*1024**3,100*1024**3))
chunks=inputs/'chunks';chunks.mkdir(exist_ok=True)
if not (inputs/'chunks-ready').exists():
    command=['/home/erikg/.cache/xsa/c3f96b9ee100ce3bbe9cdb0b19820280229373e2bc0578c7bc7c204035a68a35/chunk_frontend',str(fixture),'8',str(chunks),str(remap)]
    with (inputs/'chunk.log').open('w') as log:subprocess.run(command,env=env,stdout=log,stderr=log,check=True,preexec_fn=cap)
    (inputs/'chunks-ready').touch()
for d in ['merged','mwork']:(root/d).mkdir(exist_ok=True)
env['CROSS_V2_SPOOL_BYTES']=str(max(2*n,p*102400))
merge=[str(base/'sol7f-merge'),'--tree',str(chunks),'8',str(n),str(root/'merged/frag'),'--threads','96','--kway','8','--work',str(root/'mwork')]
finish=[str(dev/'pile_finish'),'--prefix',str(root/'merged/frag'),'--corpus',str(fixture),'--remap',str(remap),'--output',str(root/'frag.sxi2'),'--threads','96','--poll-timeout','3600','--wit-ram-budget',str(4*1024**3),'--chi-expect',{'100m':'29716349','1b':'283296933'}[size]]
(root/'box-before.txt').write_text(subprocess.check_output(['ps','-eo','pid,comm,pcpu','--sort=-pcpu'],text=True))
start=time.monotonic()
with (root/'merge.log').open('w') as ml,(root/'finish.log').open('w') as fl:
    stop=__import__('threading').Event()
    def sample():
        with (root/'sol7f-du.log').open('w') as log:
            while not stop.is_set():
                def usage(args):
                    # Concurrent band unlink may make du return 1 while it
                    # still prints a valid total. Preserve the sample and
                    # continue sampling instead of killing this thread.
                    r=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                    if r.returncode:
                        with (root/'sol7f-du-warnings.log').open('a') as warnings:
                            warnings.write(r.stderr)
                    return int(r.stdout.split()[0]) if r.stdout.split() else None
                allocated=usage(['du','-s','-B1',str(root)])
                chunk_bytes=usage(['du','-s','-B1',str(chunks)])
                apparent=usage(['du','-sb',str(root)])
                if None in (allocated,chunk_bytes,apparent):
                    stop.wait(10);continue
                log.write(f'{time.monotonic()-start} {allocated} {chunk_bytes} {allocated+chunk_bytes} {apparent}\n');log.flush()
                stop.wait(10)
    sampler=__import__('threading').Thread(target=sample);sampler.start()
    m=subprocess.Popen(merge,env=env,stdout=ml,stderr=ml,preexec_fn=cap)
    f=subprocess.Popen(finish,env=env,stdout=fl,stderr=fl,preexec_fn=cap)
    mr=m.wait();mw=time.monotonic()-start
    if mr:f.terminate()
    fr=f.wait()
    stop.set();sampler.join()
# The user-supplied 100 MB path spells mergerp. The existing bank spells
# mergperf; refuse to silently omit the exact requested path in results.
requested=base/'extcols-scratch'/('xsa-pw3-chunk-100m/mergerp-100m-snap.sxi2' if size=='100m' else 'pilev2-g2-1b/frag.sxi2')
bank=requested if requested.exists() else base/'extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2'
cmp=subprocess.run(['cmp',str(root/'frag.sxi2'),str(bank)]).returncode if not mr and not fr else None
stages={}
for name,c,s,q in re.findall(r'CROSS_STAGE_WORK stage=(\w+) pass=0 comparisons=(\d+) symbols_compared=(\d+) probes=(\d+)',(root/'merge.log').read_text()):stages[name]=list(map(int,(c,s,q)))
result=dict(size=size,P=p,kind=kind,merge=merge,finish=finish,merge_rc=mr,finish_rc=fr,cmp_rc=cmp,requested_bank=str(requested),requested_bank_exists=requested.exists(),actual_bank=str(bank),merge_wall=mw,total_wall=time.monotonic()-start,quiet_box_attested=False,oracle_replay=(p>1 and kind=='oracle'),stages=stages)
seam=re.findall(r'CROSS_SEAM_TOTAL comparisons=(-?\d+) symbols_compared=(-?\d+) probes=(-?\d+)',(root/'merge.log').read_text())
if seam:
    result['SEAM']=list(map(int,seam[-1]));result['OVERLAP']=[0,0,0]
    result['PLANNING']=stages['plan'];result['WALK']=[stages['prepare'][i]+stages['heap'][i] for i in range(3)]
    serial=stages.get('oracle_reference',[stages['heap'][i]-result['SEAM'][i] for i in range(3)])
    result['serial_whole']=[serial[i]+stages['prepare'][i] for i in range(3)]
    result['walk_whole']=[stages['heap'][i]+stages['prepare'][i] for i in range(3)]
    result['overhead']=[result['SEAM'][i]+result['PLANNING'][i] for i in range(3)]
    result['overhead_over_serial']=[result['overhead'][i]/result['serial_whole'][i] if result['serial_whole'][i] else None for i in range(3)]
    result['overhead_over_walk']=[result['overhead'][i]/result['walk_whole'][i] if result['walk_whole'][i] else None for i in range(3)]
    result['STOP_30_PERCENT']=size=='1b' and any(x is not None and x>.30 for x in result['overhead_over_walk'])
result['spool_budget_bytes']=env['CROSS_V2_SPOOL_BYTES']
result['du_columns']=['elapsed_s','output_allocated_bytes','input_chunks_allocated_bytes','total_allocated_bytes','output_apparent_bytes']
(root/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)
assert mr==fr==cmp==0
