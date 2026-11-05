#!/usr/bin/env python3
"""Real kill/restart integration gate. No deadlines; only kills its own descendants."""
import argparse, fcntl, hashlib, json, os, pathlib, signal, subprocess, time
p=argparse.ArgumentParser();p.add_argument('binary');p.add_argument('corpus');p.add_argument('root');a=p.parse_args()
root=pathlib.Path(a.root);root.mkdir(parents=True,exist_ok=True)
binary=str(pathlib.Path(a.binary).resolve()); corpus=str(pathlib.Path(a.corpus).resolve())
def sha(path):
 h=hashlib.sha256()
 with open(path,'rb') as f:
  for b in iter(lambda:f.read(4<<20),b''):h.update(b)
 return h.hexdigest()
def command(scratch, inp=corpus):return [binary,'build','--tape','--input',inp,'--scratch',str(scratch),'--threads','16','--memory-gb','24','--with-chi']
def launch(scratch,label):
 with open(root/(label+'.log'),'wb') as log:return subprocess.Popen(command(scratch),stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
def output(scratch):return scratch/(pathlib.Path(corpus).stem+'.sxi2')
def await_pieces(proc,scratch):
 while True:
  if proc.poll() is not None:raise RuntimeError(f'build exited before interruption: {proc.returncode}')
  journal=scratch/'checkpoint.journal'
  if journal.exists() and journal.read_text().count('PIECE_DURABLE ')>=1500:return
  time.sleep(.1)
def children(pid):
 try:direct=[int(x) for x in pathlib.Path(f'/proc/{pid}/task/{pid}/children').read_text().split()]
 except FileNotFoundError:return []
 return [q for c in direct for q in [c,*children(c)]]
def kill_tree(proc):
 # Freeze the front door before discovering descendants. Freeze each descendant
 # and rescan: no unobserved fork can survive the final stable tree snapshot.
 seen=set();os.kill(proc.pid,signal.SIGSTOP)
 while True:
  live=set(children(proc.pid));fresh=live-seen
  if not fresh:break
  for pid in fresh:
   try:os.kill(pid,signal.SIGSTOP)
   except ProcessLookupError:pass
  seen |= fresh
 for pid in sorted(seen,reverse=True):
  try:os.kill(pid,signal.SIGKILL)
  except ProcessLookupError:pass
 os.kill(proc.pid,signal.SIGKILL);assert proc.wait()==-signal.SIGKILL
 # Wait on the inherited build lock rather than /proc fd enumeration (zombie
 # fd directories can be unreadable even to their former owner).
 lock=root/('.'+current_scratch.name+'.xsa-build.lock')
 with open(lock,'rb') as f:
  while True:
   try:fcntl.flock(f,fcntl.LOCK_EX|fcntl.LOCK_NB);break
   except BlockingIOError:time.sleep(.05)
 return len(seen)
results={}
s=root/'uninterrupted';proc=launch(s,'uninterrupted');assert proc.wait()==0;reference=sha(output(s));results['uninterrupted_sha256']=reference
for mode in ['kill','term']:
 s=root/mode;current_scratch=s;proc=launch(s,mode+'-first');await_pieces(proc,s)
 if mode=='kill':results['killed_children']=kill_tree(proc)
 else:
  os.kill(proc.pid,signal.SIGTERM);rc=proc.wait();assert rc==75,rc
  assert 'BUILD_INTERRUPTED ' in (s/'checkpoint.journal').read_text();results['sigterm_rc']=rc
 if mode=='kill':
  pieces=s/'merged'/'frag.pieces';pieces.mkdir(parents=True,exist_ok=True)
  (pieces/'p-999999-0').write_bytes(b'unjournalled')
  (pieces/'p-999999-0.partial').write_bytes(b'incomplete')
  # Wrong input must be refused before altering ANY scratch file.
  wrong=root/'wrong.bin'
  with open(wrong,'wb') as f:f.write(b'different corpus')
  def snapshot():return {str(f.relative_to(s)):sha(f) for f in s.rglob('*') if f.is_file()}
  before=snapshot();r=subprocess.run(command(s,str(wrong)),capture_output=True,text=True)
  assert r.returncode!=0 and 'field=corpus_sha256' in r.stderr,r.stderr
  assert snapshot()==before;results['plan_refusal']=r.stderr.strip()
 proc=launch(s,mode+'-resume');assert proc.wait()==0
 digest=sha(output(s));assert digest==reference,(mode,digest,reference);results[mode+'_sha256']=digest
 print(json.dumps(results),flush=True)
(root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
