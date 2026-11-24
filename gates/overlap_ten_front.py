"""Meter the 10 GB raw packed-column census separately. Not a product gate."""
import json
from pathlib import Path
import random
import re
import subprocess as sp
import sys
import tempfile
import time
from overlap_resident_gate import CG, cg_info, finished, footprint, proc_info, save
from overlap_resident_test import prepare, trigger

N=10_000_000_000

def fixtures(exe):
    rng=random.Random(191); tr=next(x.to_bytes(8,'big') for x in range(2000) if trigger(x.to_bytes(8,'big')))
    cases=[b'x',b'\0'+tr,tr*60+tr[:7],bytes(range(256))*20,rng.randbytes(20000),b'A'*33000]
    for text in cases:
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);prepare(d,text); out=d/'new';cmd=[exe,d/'corpus',out]
            assert sp.run(cmd,capture_output=True).returncode==70 and not out.exists()
            p=sp.run([*cmd,'--fixture'],capture_output=True);assert p.returncode==0,p.stderr
            assert {f.name for f in out.iterdir()}=={'parse.u32','dictionary.packed'}
            for f in out.iterdir():assert f.read_bytes()==(d/'columns'/f.name).read_bytes()
            assert sp.run([*cmd,'--fixture'],capture_output=True).returncode==70
            (d/'stop.requested').touch();assert sp.run([exe,d/'corpus',d/'stopped','--fixture'],capture_output=True).returncode==75
    print('PASS compact lexical buckets: 6 independent byte-exact word tapes and ID strings; wrong rung, reuse and stop refused')

def build(exe,d):
    assert (CG/'memory.max').read_text().strip()==str(192<<20) and (CG/'memory.swap.max').read_text().strip()=='0' and (CG/'cpu.max').read_text().strip()=='100000 100000'
    corpus=d/'in/corpus';assert corpus.stat().st_size==N
    start=time.monotonic(); rss=alloc=hi=0
    with (d/'front.log').open('xb') as log:
        p=sp.Popen([exe,corpus,d/'columns'],stdout=log,stderr=log)
        with open(f'/proc/{p.pid}/io') as io:
            while not finished(p):
                info=proc_info(p.pid,io);rss=max(rss,info['VmHWM']); total,parts=footprint(d,0);alloc=max(alloc,total);hi=max(hi,sum(parts.values()))
                if hi>2*N or time.monotonic()-start>2400 or info['read_bytes']>6*N:(d/'stop.requested').touch(exist_ok=True)
                time.sleep(.8)
            info=proc_info(p.pid,io);rc=p.wait()
    record=dict(rc=rc,wall_s=time.monotonic()-start,process=info,cgroup=cg_info(),sampled_peak_rss=rss,sampled_counted_bytes=hi,sampled_dir_allocated=alloc)
    save(d/'front-end.json',record)
    if rc!=0 or (d/'stop.requested').exists():raise SystemExit(rc if rc>0 else 75)
    assert 'oom_kill 0' in record['cgroup']['memory.events'].splitlines()
    line=next(l for l in (d/'front.log').read_text().splitlines() if l.startswith('LEX10 ')); v={k:int(x) for k,x in re.findall(r'(\w+)=(\d+)',line)}
    m,k,D=v['m'],v['k'],v['D'];cols={p.name:p.stat().st_size for p in (d/'columns').iterdir()}
    assert v['n']==N and cols=={'parse.u32':4*m,'dictionary.packed':v['dictionary_bytes']} and v['starts_above_u32']>0 and D>2**32 and v['dictionary_bytes']==D+3*k-1
    assert sum(cols.values())<=v['peak_counted']<=2*N and hi<=2*N
    # Proposed columns, not a measurement of an implemented 10 GB sort:
    # u64 byte positions and LCP; u32 IDs and parse ranks, separate arrays.
    retained=12*(k+1)+21*m+16*((m+15)//16)
    model=dict(parse=cols['dictionary.packed']+8*m+40*k,aux=cols['dictionary.packed']+D+33*(k+1)+56*(m+1),
               sort=9*D+retained+(64<<20),emit=9*D+16*((D+63)//64)+retained+40*k+(8<<20))
    record.update(census=v,columns=cols,parse_sort_extent=m+6*k,proposed_array_bytes=model,bitmap_bytes=(N+1+7)//8,
                  worker_scope='raw input to packed columns only; no suffix ordering, row or container check')
    save(d/'front-end.json',record);print('TEN_FRONT',line,'wall_s',round(record['wall_s'],2),'parse_extent',m+6*k,'modeled_max_array_bytes',max(model.values()),flush=True)
    if m+6*k>=2**31 or v['alphabet']>254:raise SystemExit(75)

if __name__=='__main__':
    if sys.argv[1]=='test':fixtures(Path(sys.argv[2]))
    elif sys.argv[1]=='build':build(Path(sys.argv[2]),Path(sys.argv[3]))
    else:raise SystemExit(64)
