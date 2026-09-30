#!/usr/bin/env python3
"""Paired HTTP byte parity and warm locate query timing for published SXI files."""
import argparse
import hashlib
import json
import pathlib
import socket
import statistics
import subprocess
import time
import urllib.request

p=argparse.ArgumentParser()
p.add_argument('--xsa',required=True)
p.add_argument('--old',required=True)
p.add_argument('--new',required=True)
p.add_argument('--pattern',required=True)
p.add_argument('--label',required=True)
p.add_argument('--mode',choices=['dna','text'],required=True)
p.add_argument('--reps',type=int,default=20)
p.add_argument('--sample',type=int,default=16)
a=p.parse_args()
root=pathlib.Path(__file__).parent

def query(path,side):
    with socket.socket() as s:
        s.bind(('127.0.0.1',0));port=s.getsockname()[1]
    err=(root/f'{a.label}-{side}-serve.log').open('wb')
    proc=subprocess.Popen([a.xsa,'serve','--sxi',path,'--bind',f'127.0.0.1:{port}',
                           '-j','1','--mode',a.mode,'--sample',str(a.sample),'--seed','7'],
                          stdout=subprocess.DEVNULL,stderr=err)
    base=f'http://127.0.0.1:{port}'
    try:
        deadline=time.monotonic()+900
        while time.monotonic()<deadline:
            if proc.poll() is not None:raise RuntimeError(f'server exited {proc.returncode}')
            try:
                urllib.request.urlopen(base+'/stats',timeout=1).read();break
            except OSError:time.sleep(.1)
        else:raise RuntimeError('server did not become ready')
        body=json.dumps({'pattern':a.pattern}).encode()
        times=[];first=None
        for _ in range(a.reps):
            req=urllib.request.Request(base+'/query',body,{'Content-Type':'application/json'})
            t=time.perf_counter();response=urllib.request.urlopen(req,timeout=120).read()
            times.append(time.perf_counter()-t)
            if first is None:first=response
            elif response!=first:raise AssertionError('unstable HTTP response')
        (root/f'{a.label}-{side}-http.jsonl').write_bytes(first)
        return first,{'requests':a.reps,'hits':len(first.splitlines()),
                      'bytes':len(first),'sha256':hashlib.sha256(first).hexdigest(),
                      'median_ms':1000*statistics.median(times),
                      'mean_ms':1000*statistics.mean(times),
                      'requests_per_second':a.reps/sum(times)}
    finally:
        proc.terminate()
        try:proc.wait(timeout=10)
        except subprocess.TimeoutExpired:proc.kill();proc.wait()
        err.close()

old,old_stat=query(a.old,'old')
new,new_stat=query(a.new,'new')
result={'label':a.label,'pattern':a.pattern,'sample':a.sample,'byte_parity':old==new,
        'old':old_stat,'new':new_stat}
(root/f'{a.label}-http-gate.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
if old!=new or not old:raise SystemExit(1)
