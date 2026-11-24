"""Measure the exact sink codec on paired slices, not a construction speedup."""
import json
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import time
from overlap_resident_gate import cg_info,finished,proc_info,save

N=100_000_503

def meter(d,exe,ram):
    before=cg_info();assert int(before['memory.max'])==192<<20 and before['memory.swap.max']=='0' and before['cpu.max']=='100000 100000'
    assert ram.parent==Path('/dev/shm') and ram.stat().st_uid==os.getuid() and not list(ram.iterdir())
    with (d/'codec.log').open('xb') as log:
        start=time.monotonic();p=sp.Popen([exe,'--exact','unit::sxls_batch_test::paired_live_slices','--ignored','--nocapture','--test-threads=1'],env=dict(os.environ,XSA_SXLS_LAYOUT_ROOT=str(ram)),stdout=log,stderr=log)
        with open(f'/proc/{p.pid}/io') as io:
            while not finished(p):
                if (d/'stop.requested').exists() or time.monotonic()-start>300:(ram/'stop').touch(exist_ok=True);(d/'stop.requested').touch(exist_ok=True)
                time.sleep(.2)
            stats=proc_info(p.pid,io);rc=p.wait()
    rec=dict(rc=rc,wall_including_wait_s=time.monotonic()-start,process=stats,cgroup=cg_info());save(d/'codec.json',rec);assert rc==0 and not list(ram.iterdir()) and 'oom 0' in rec['cgroup']['memory.events'].splitlines()
    line=re.search(r'SXLS_TRIAL ([^\n]+)',(d/'codec.log').read_text())[1];rec['trial']={k:(float(v) if k.endswith('_s') else int(v)) for k,v in re.findall(r'(\w+)=([\d.]+)',line)};save(d/'codec.json',rec)

def score(d):
    read=lambda x:json.loads(x.read_text());root=Path(__file__).resolve().parent/'receipts';ref=read(root/'radix-emit-100m/result.json');b=read(d/'metrics.json');s=read(d/'codec.json');v=s['trial']
    assert v['rows']==ref['rows']==N+1 and b['digest']==ref['digest'] and s['rc']==0 and v['exact_bytes']==1
    assert b['lanes']==4 and b['rc']==dict(hash=0,producer=0) and v['slices']==(N+262144)//262144
    bound=b['counted_scratch_bound']+2*(48+17*262144);assert v['peak_pair_logical']<=2*(48+17*262144) and bound<=2*N
    expected=2*v['escapes']-v['nonempty'];observed=v['scalar_syscw']-v['batch_syscw'];assert observed>=.99*expected
    rec=dict(scope='paired actual SXLS encode/write/sync/rename of every slice; real 100 MB row stream, private tmpfs, no finish',digest=b['digest'],
             joiner_and_other_scratch=b['counted_scratch_bound'],paired_scratch_bound=bound,limit=2*N,slices=v['slices'],escapes=v['escapes'],
             scalar_s=v['scalar_s'],batch_s=v['batch_s'],local_wall_speedup=v['scalar_s']/v['batch_s'],scalar_syscw=v['scalar_syscw'],batch_syscw=v['batch_syscw'],predicted_saved_syscw=expected,observed_saved_syscw=observed)
    save(d/'result.json',rec);print('SXLS_RESULT',rec,flush=True)
if __name__=='__main__':
    if sys.argv[1]=='meter':meter(*(Path(s) for s in sys.argv[2:]))
    elif sys.argv[1]=='score':score(Path(sys.argv[2]))
    else:raise SystemExit(64)
