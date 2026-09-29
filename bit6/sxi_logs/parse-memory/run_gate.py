#!/usr/bin/env python3
import json, os, pathlib, shutil, subprocess, sys, time
HERE=pathlib.Path(__file__).resolve().parent
WORK=HERE/'work'; BIN=WORK/'build/rpfbwt-memory'
CASES={'yeast':('/tmp/rpfbwt-64-yeast/parse',['.rlebwt','.rlebwt.meta','.ssa','.ssa_t']),
       'k10':('/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp',['.rlebwt','.rlebwt.meta','.ssa'])}
def record(**event):
    with (HERE/'gates.jsonl').open('a') as f:f.write(json.dumps(dict(time=time.time(),**event))+'\n')
def run(tag,cmd):
    record(stage=tag,status='START',command=list(map(str,cmd)))
    with (HERE/(tag+'.log')).open('w') as f:
        p=subprocess.Popen(['/usr/bin/time','-v','-o',str(HERE/(tag+'.time')),'/usr/bin/prlimit','--as=350000000000',*map(str,cmd)],stdout=f,stderr=f,env=dict(os.environ,OMP_NUM_THREADS='48'))
        record(stage=tag,pid=p.pid)
        rc=p.wait()
    record(stage=tag,status='PASS' if not rc else 'FAIL',returncode=rc)
    if rc:raise RuntimeError(f'{tag}: exit {rc}')
def main():
    case=sys.argv[1]; ref,exts=CASES[case]; dest=WORK/case;dest.mkdir(exist_ok=True);prefix=dest/'parse'
    for ext in ['.dict','.parse','.parse.dict','.parse.parse']:
        subprocess.run(['cp','--reflink=auto',ref+ext,str(prefix)+ext],check=True)
    run(case,[BIN,'--l1-prefix',prefix,'--w1','10','--w2','5','--threads','48','--chunks','50','--tmp-dir',dest])
    for ext in exts:
        subprocess.run(['cmp',ref+ext,str(prefix)+ext],check=True)
        record(stage=case,status='BYTE_IDENTICAL',extension=ext,reference=ref)
    record(stage=case,status='GATE_PASS')
if __name__=='__main__':
    try:main()
    except Exception as e:record(stage=sys.argv[1],status='GATE_FAIL',error=str(e));raise
