#!/usr/bin/env python3
"""Run isolated optional-chi tape stages; retain commands, timings and progress."""
import argparse, json, os, re, signal, shutil, subprocess, time
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument('--root', type=Path, required=True)
p.add_argument('--bin', type=Path, required=True)
p.add_argument('--corpus', type=Path, required=True)
p.add_argument('--remap', type=Path)
p.add_argument('--pending-bands', type=int, default=8)
p.add_argument('--threads', type=int, default=32)
p.add_argument('--memory-gb', type=int, default=96)
p.add_argument('--with-chi', action='store_true')
p.add_argument('--keep-lcp', action='store_true', help='Keep production-default LCP slices with chi absent')
p.add_argument('--stop-percent', type=float, default=0)
p.add_argument('--seconds', type=int, default=2400)
a=p.parse_args(); a.root.mkdir(parents=True,exist_ok=False)
for d in ['bands','waves']: (a.root/d).mkdir()
remap=a.root/'remap'; prefix=a.root/'frag'
if a.remap: shutil.copyfile(a.remap,remap)
else: subprocess.run([str(a.bin/'xsa_finish'),'--corpus',str(a.corpus),'--sentinel-remap',str(remap)],check=True)
base=['--corpus',str(a.corpus),'--remap',str(remap)]
cmds=[
 [str(a.bin/'scatter_sort'),*base,'--scratch',str(a.root/'waves'),'--tape-bands',str(a.root/'bands'),'--pending-bands',str(a.pending_bands),'--budget-bytes',str((a.memory_gb//2)<<30),'--memory-gb',str(a.memory_gb),'--threads',str(a.threads)],
 [str(a.bin/'cross_lcp_merge'),'--binbwt',str(a.corpus),'--remap',str(remap),'--out-prefix',str(prefix),'--threads',str(a.threads),'--tape-bands',str(a.root/'bands')],
 [str(a.bin/'xsa_finish'),'--prefix',str(prefix),*base,'--output',str(a.root/'product.sxi2'),'--threads',str(a.threads),'--poll-timeout',str(a.seconds+60),'--with-chi' if a.with_chi else '--no-chi']]
if a.with_chi or a.keep_lcp: cmds[2] += ['--lcp-bands',str(a.root/'bands')]
else: cmds[0] += ['--no-lcp-slices']
(a.root/'commands.json').write_text(json.dumps(cmds,indent=2))
env=dict(os.environ,CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='1100',CROSS_POOL_SLOTS='33554432')
ps=[]; logs=[]; start=time.monotonic(); reason='deadline'; progress=0
try:
 for tag,cmd in zip(['order','emit','finish'],cmds):
  log=(a.root/(tag+'.log')).open('w');logs.append(log)
  ps.append(subprocess.Popen(['/usr/bin/time','-v','-o',str(a.root/(tag+'.time')),*cmd],env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True))
 while time.monotonic()-start<a.seconds:
  if any(x.poll() not in (None,0) for x in ps): reason='stage-failure';break
  if all(x.poll()==0 for x in ps): reason='complete';break
  txt=(a.root/'finish.log').read_text(errors='replace')
  matches=re.findall(r'XSA_FINISH_PROGRESS band=(\d+)/(\d+) runs=(\d+) chi_so_far=(\d+)',txt)
  if matches:
   b,total,r,chi=map(int,matches[-1]);progress=100*b/total if total else 0
   (a.root/'progress.json').write_text(json.dumps(dict(band=b,bands=total,runs=r,chi=chi,band_percent=progress,wall=time.monotonic()-start)))
   if a.stop_percent and progress>=a.stop_percent: reason='requested-progress';break
  time.sleep(2)
finally:
 for x in ps:
  if x.poll() is None: os.killpg(x.pid,signal.SIGTERM)
 for x in ps:
  try:x.wait(timeout=5)
  except subprocess.TimeoutExpired:os.killpg(x.pid,signal.SIGKILL);x.wait()
 for f in logs:f.close()
 result=dict(reason=reason,returncodes=[x.returncode for x in ps],wall=time.monotonic()-start,band_percent=progress,chi_present=a.with_chi)
 out=a.root/'product.sxi2'
 if out.exists():result['container_bytes']=out.stat().st_size
 (a.root/'result.json').write_text(json.dumps(result,indent=2));print(json.dumps(result),flush=True)
if reason not in ['complete','requested-progress']:raise SystemExit(1)
