#!/usr/bin/env python3
"""After the memory K10 producer exits, obtain its missing fourth oracle.
The historical retained K10 output has no .ssa_t. A separate private baseline
run supplies all four outputs without touching retained artifacts or running
another 48-thread front end concurrently with the memory K10 producer.
"""
import json, pathlib, subprocess, time
from run_gate import HERE, WORK, record, run
record(stage='k10-four-file-continuation',status='WAITING_FOR_MEMORY_GATE')
while True:
    events=[json.loads(line) for line in (HERE/'gates.jsonl').read_text().splitlines()]
    statuses=[e.get('status') for e in events if e.get('stage')=='k10']
    if 'GATE_FAIL' in statuses:
        record(stage='k10-all-four',status='SKIPPED',reason='memory gate failed');raise SystemExit(1)
    if 'GATE_PASS' in statuses:break
    time.sleep(15)
try:
    dest=WORK/'k10-baseline';dest.mkdir(exist_ok=True);prefix=dest/'parse'
    reference='/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp'
    for ext in ['.dict','.parse','.parse.dict','.parse.parse']:
        subprocess.run(['cp','--reflink=auto',reference+ext,str(prefix)+ext],check=True)
    run('k10-baseline',['/home/erikg/sxgc/sealed-tools/rpfbwt','--l1-prefix',prefix,'--w1','10','--w2','5','--threads','48','--chunks','50','--tmp-dir',dest])
    for ext in ['.rlebwt','.rlebwt.meta','.ssa','.ssa_t']:
        subprocess.run(['cmp',str(prefix)+ext,str(WORK/'k10/parse')+ext],check=True)
        record(stage='k10-all-four',status='BYTE_IDENTICAL',extension=ext)
    record(stage='k10-all-four',status='GATE_PASS')
except Exception as e:
    record(stage='k10-all-four',status='GATE_FAIL',error=str(e));raise
