#!/usr/bin/env python3
"""No-clobber full SXI2 v6-2 conversion, byte parity, and warm HTTP gates."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time

p = argparse.ArgumentParser()
p.add_argument('--writer', required=True)
p.add_argument('--xsa', required=True)
p.add_argument('--dest', required=True)
p.add_argument('--corpus', choices=['yeast235', 'pile-frag', 'k10'], required=True)
a = p.parse_args()
here = Path(__file__).parent
v5 = here.parent / 'sxi2-v5'
pre = {x['artifact']: x for x in json.loads((here/'space-preflight.json').read_text())}
sources = {Path(x['path']).stem: Path(x['path']) for x in json.loads((here.parent/'sxi2-v2/size-probe.json').read_text())}
row = pre[a.corpus]
source = sources[a.corpus]
dest = Path(a.dest)
dest.mkdir(exist_ok=True)
output = dest / (a.corpus+'.sxi2')
label = {'yeast235':'yeast', 'pile-frag':'pile', 'k10':'k10'}[a.corpus]
mode = 'text' if a.corpus=='pile-frag' else 'dna'
reads = v5 / (a.corpus+'.reads2.fa')

def run(tag, cmd, stdout=None):
    log = here/(a.corpus+'-'+tag+'.log')
    if log.exists(): raise RuntimeError('gate log exists: '+str(log))
    t = time.monotonic()
    with log.open('wb') as err:
        result = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=err)
    if stdout is not None: stdout.write_bytes(result.stdout)
    (here/(a.corpus+'-'+tag+'.time.json')).write_text(json.dumps({'command':cmd,'wall_seconds':time.monotonic()-t,'exit':result.returncode},indent=2)+'\n')
    if result.returncode: raise RuntimeError(f'{tag} failed: {result.returncode}; see {log}')
    return result.stdout

if not output.exists():
    run('convert', [a.writer,'--sxi',str(source),'--output',str(output),
                    '--validator',a.xsa,'--max-bytes',str(row['v5_bytes']-1)])
raw = output.open('rb').read(8)
assert raw[:4]==b'SXI2' and int.from_bytes(raw[4:8],'little')==4, raw
assert output.stat().st_size==row['projected_bytes'], (output.stat().st_size,row['projected_bytes'])
new = here/(label+'-native-new.jsonl')
run('native', [a.xsa,'mems','--sxi',str(output),'--reads',str(reads),
               '--min-len','50','-j','1','--mode',mode],new)
old = (v5/(label+'-native-old.jsonl')).read_bytes()
assert new.read_bytes()==old, 'full native byte parity'
if a.corpus!='k10':
    bounded = here/(label+'-bounded-new.tsv')
    run('bounded',[a.xsa,'mems','--sxi',str(output),'--reads',str(reads),
                   '--min-len','20','-j','1','--mode',mode,'--out','ropebwt3','-p','10'],bounded)
    old_bounded = v5/(label+'-mems-old.tsv')
    if not old_bounded.exists(): old_bounded=v5/(label+'-mems2-old.tsv')
    assert bounded.read_bytes()==old_bounded.read_bytes(), 'bounded byte parity'
reference = a.corpus+'-bench-http-gate.json' if a.corpus=='pile-frag' else a.corpus+'-http-gate.json'
http = json.loads((v5/reference).read_text())
cmd=['python3',str(here/'http_gate.py'),'--xsa',a.xsa,'--old',str(source),'--new',str(output),
     '--pattern',http['pattern'],'--label',a.corpus,'--mode',mode,'--reps',str(200 if a.corpus=='pile-frag' else 5)]
if a.corpus=='k10':cmd+=['--startup-timeout','2400']
run('http',cmd)
gate=json.loads((here/(a.corpus+'-http-gate.json')).read_text())
assert gate['byte_parity'] and gate['old']['sha256']==gate['new']['sha256'], 'HTTP byte parity'
print(json.dumps({'corpus':a.corpus,'version':4,'bytes':output.stat().st_size,
                  'native_sha256':hashlib.sha256(new.read_bytes()).hexdigest(),
                  'http_median_ms':gate['new']['median_ms'],'http_v5_median_ms':http['new']['median_ms']},indent=2))
