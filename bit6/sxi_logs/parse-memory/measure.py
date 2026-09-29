#!/usr/bin/env python3
"""Reproducible parse-regime experiment; all artifacts confined to work/."""
import json, pathlib, subprocess, time
import numpy as np
HERE = pathlib.Path(__file__).resolve().parent
WORK = HERE/'work'
PFP = str(WORK/'pfp-probe')
def record(obj):
    with (HERE/'measurements.jsonl').open('a') as f:
        f.write(json.dumps(obj)+'\n')
def run(tag, args):
    start=time.monotonic()
    with (HERE/(tag+'.log')).open('w') as f:
        r=subprocess.run(['/usr/bin/time','-v','-o',str(HERE/(tag+'.time')),'/usr/bin/prlimit','--as=150000000000',*args],stdout=f,stderr=f)
    record(dict(stage=tag,command=args,returncode=r.returncode,wall_s=time.monotonic()-start))
    r.check_returncode()
def main():
    for size in (100_000_000,500_000_000,1_000_000_000):
        inp=WORK/f'pile-{size}.txt'
        with (WORK/'pile-frag.txt').open('rb') as f, inp.open('wb') as out:
            left=size; removed=0
            while left:
                chunk=f.read(min(left,8*1024*1024))
                if not chunk: break
                left-=len(chunk); clean=chunk.translate(None,bytes(range(6)));removed+=len(chunk)-len(clean);out.write(clean)
        for w,p in ((10,100),(5,100),(20,100),(3,5)):
            tag=f'parse-{size}-w{w}-p{p}';prefix=WORK/tag
            run(tag,[PFP,'-t',str(inp),'-o',str(prefix),'-w',str(w),'-p',str(p),'--output-occurrences'])
            d=np.memmap(str(prefix)+'.dict',dtype=np.uint8,mode='r')
            ends=np.flatnonzero(d==1);lengths=np.diff(np.r_[-1,ends])-1
            result=dict(stage='measurement',tag=tag,input_bytes=inp.stat().st_size,removed_bytes=removed,w=w,p=p,
                dictionary_bytes=len(d),distinct_phrases=len(ends),parse_bytes=pathlib.Path(str(prefix)+'.parse').stat().st_size,
                D_over_n=len(d)/inp.stat().st_size,phrase_length_mean=float(np.mean(lengths)),
                phrase_length_quantiles=dict(zip(('min','p50','p90','p99','max'),map(float,np.quantile(lengths,[0,.5,.9,.99,1])))))
            result['parse_entries']=result['parse_bytes']//4
            record(result)
            del d
if __name__=='__main__':main()
