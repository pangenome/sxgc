#!/usr/bin/env python3
"""Run isolated tape stages and persist waits, band timings and exit evidence."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import math
import statistics
import subprocess
import time

p=argparse.ArgumentParser(description=__doc__)
for name in ['root','corpus','remap','sorter','merge','finish']:
    p.add_argument('--'+name,required=True)
p.add_argument('--threads',type=int,default=96)
p.add_argument('--pending-bands',type=int,default=8)
p.add_argument('--seconds',type=int,default=1800)
p.add_argument('--stop-band',type=int)
p.add_argument('--chi',type=int)
a=p.parse_args()
root=Path(a.root);root.mkdir(parents=True)
for d in ['bands','waves']: (root/d).mkdir()
prefix=str(root/'frag');base=['--corpus',a.corpus,'--remap',a.remap]
cmds=[[a.sorter,*base,'--scratch',str(root/'waves'),'--tape-bands',str(root/'bands'),
       '--pending-bands',str(a.pending_bands),'--budget-bytes',str(48<<30),'--memory-gb','96',
       '--threads',str(a.threads),'--no-lcp-slices'],
      [a.merge,'--binbwt',a.corpus,'--remap',a.remap,'--out-prefix',prefix,
       '--threads',str(a.threads),'--tape-bands',str(root/'bands')],
      [a.finish,'--prefix',prefix,*base,'--output',str(root/'product.sxi2'),
       '--threads',str(a.threads),'--poll-timeout',str(a.seconds+60),
       '--wit-ram-budget',str(48<<30)]]
if a.chi is not None:cmds[2]+=['--chi-expect',str(a.chi)]
env=dict(os.environ,XSA_TAPE_WAIT_S='180',CROSS_V2_PIECES='1',
         CROSS_V2_PIECE_WINDOW='1100',CROSS_POOL_SLOTS='33554432',XSA_LCE_VERIFY='1')
ps=[];logs=[];start=time.monotonic();reason='deadline'
try:
    for tag,cmd in zip(['order','emit','finish'],cmds):
        log=(root/(tag+'.log')).open('w');logs.append(log)
        ps.append(subprocess.Popen(cmd,stdout=log,stderr=subprocess.STDOUT,stdin=subprocess.DEVNULL,
                                   start_new_session=True,env=env))
    (root/'commands.json').write_text(json.dumps(cmds,indent=2))
    while time.monotonic()-start<a.seconds:
        if any(proc.poll() not in (None,0) for proc in ps):reason='stage-failure';break
        if all(proc.poll()==0 for proc in ps):reason='complete';break
        if a.stop_band is not None and (root/'bands'/f'band_{a.stop_band}.consumed').exists():
            reason='requested-band-reached';break
        time.sleep(.2)
except KeyboardInterrupt:
    reason='operator-interrupt'
finally:
    for proc in ps:
        if proc.poll() is None:os.killpg(proc.pid,signal.SIGTERM)
    for proc in ps:
        try:proc.wait(timeout=10)
        except subprocess.TimeoutExpired:os.killpg(proc.pid,signal.SIGKILL);proc.wait()
    for log in logs:log.close()
    order=(root/'order.log').read_text();finish=(root/'finish.log').read_text()
    waits=[float(x) for x in re.findall(r'SS_WAIT_DONE .*wall_s=([\d.]+)',order)]
    timings=[dict(band=int(b),rows=int(n),read_s=float(r),sort_s=float(s),write_s=float(w))
             for b,n,r,s,w in re.findall(r'SS_BAND band=(\d+) rows=(\d+) read_s=([\d.]+) sort_s=([\d.]+) write_s=([\d.]+)',order)]
    result=dict(reason=reason,seconds=time.monotonic()-start,returncodes=[proc.returncode for proc in ps],
        max_observed_wait_s=max(map(float,re.findall(r'SS_WAIT(?:_DONE)? .*wall_s=([\d.]+)',order)),default=0),
        max_completed_wait_s=max(waits,default=0),top5_sort=sorted(timings,key=lambda x:x['sort_s'],reverse=True)[:5],
        published_bands=len(timings),consumed_bands=len(list((root/'bands').glob('band_*.consumed'))),
        splits=[line for line in order.splitlines() if 'SS_SPLIT' in line],
        finish_done=[line for line in finish.splitlines() if 'XSA_FINISH_DONE' in line],
        incomplete_waits='SS_WAIT ' in order and reason!='complete')
    hist={str(d):sum(int(v) for v in re.findall(rf' depth{d}=(\d+)',order))
          for d in range(24,193,24)}
    times=sorted(t['sort_s'] for t in timings)
    median=statistics.median(times) if times else 0
    p99=times[math.ceil(.99*len(times))-1] if times else 0
    result.update(depth_histogram=hist,order_analytic_leaves=order.count('order=analytic'),
        tape_leaf_at_cap=order.count('TAPE_LEAF_AT_CAP'),sort_median_s=median,
        sort_p99_s=p99,sort_p99_median_ratio=p99/median if median else None,
        max_estimated_band_rows=max(map(int,re.findall(r'max_estimated_rows=(\d+)',order)),default=0),
        max_completed_band_rows=max(map(int,re.findall(r'TAPE_ROW_BOUND .* rows=(\d+)',order)),default=0),
        row_bound_violations=len(re.findall(r'TAPE_ROW_BOUND .* ok=0',order)))
    if reason=='complete':
        h=hashlib.sha256()
        with (root/'product.sxi2').open('rb') as f:
            for block in iter(lambda:f.read(8<<20),b''):h.update(block)
        result['product_sha256']=h.hexdigest()
    (root/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2),flush=True)
if reason not in ('complete','requested-band-reached'):raise SystemExit(1)
