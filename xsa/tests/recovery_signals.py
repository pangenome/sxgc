#!/usr/bin/env python3
"""Check SIGINT/SIGHUP drain behavior on the 150 MB gate corpus."""
import argparse, json, os, pathlib, signal, subprocess, time
parser=argparse.ArgumentParser()
parser.add_argument('binary');parser.add_argument('corpus');parser.add_argument('root')
a=parser.parse_args();root=pathlib.Path(a.root);root.mkdir(parents=True,exist_ok=True)
results={}
for sig in [signal.SIGINT,signal.SIGHUP]:
 scratch=root/sig.name
 command=[str(pathlib.Path(a.binary).resolve()),'build','--tape','--input',str(pathlib.Path(a.corpus).resolve()),'--scratch',str(scratch),'--threads','16','--memory-gb','24','--with-chi']
 with open(root/(sig.name+'.log'),'wb') as log:
  proc=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
 while True:
  assert proc.poll() is None, f'build finished before {sig.name}'
  journal=scratch/'checkpoint.journal'
  if journal.exists() and journal.read_text().count('PIECE_DURABLE ')>=20:break
  time.sleep(.1)
 os.kill(proc.pid,sig);rc=proc.wait();assert rc==75,(sig.name,rc)
 records=journal.read_text().splitlines()
 interrupted=[s for s in records if s.startswith('BUILD_INTERRUPTED ')]
 assert len(interrupted)==1 and f'reason={int(sig)} ' in interrupted[0]
 assert 'BUILD_DONE ' not in journal.read_text()
 results[sig.name]={'exit_code':rc,'record':interrupted[0]}
 print(json.dumps(results),flush=True)
(root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
