#!/usr/bin/env python3
"""Real battery through an installed xsa, with exact endpoint/aggregate gates."""
import argparse
import filecmp
import json
from pathlib import Path
import subprocess
import tempfile
p=argparse.ArgumentParser()
p.add_argument('--xsa',type=Path,required=True)
p.add_argument('--fixtures',type=Path,required=True)
p.add_argument('--oracles',type=Path,required=True)
p.add_argument('--log-dir',type=Path,required=True)
a=p.parse_args();a.log_dir.mkdir(parents=True,exist_ok=True)
work=Path(tempfile.mkdtemp(prefix='xsa-cargo-battery-'))
env={'PATH':'/usr/bin:/bin','HOME':str(work/'home'),'LANG':'C.UTF-8'}
results=[]
for name in ('duplicates-600k','random-4-2k','random-4-20k','random-bin-20k',
             'random-4-200k','satellite-18k','HOR-nested'):
    cmd=[str(a.xsa.resolve()),'build','--text',str(a.fixtures/(name+'.txt')),
         '-o',str(work/(name+'.sxi')),'--scratch',str(work),'--threads','2',
         '--expect-heads',str(a.oracles/(name+'.head_sa')),
         '--expect-ri4',str(a.fixtures/(name+'.ri4')),'--log-dir',str(a.log_dir)]
    r=subprocess.run(cmd,cwd=work,env=env,capture_output=True,text=True)
    (a.log_dir/(name+'.log')).write_text(r.stdout+r.stderr)
    assert r.returncode==0,(name,r.stderr)
    result=json.loads(r.stdout.splitlines()[-1])
    assert filecmp.cmp(Path(result['work'])/'fresh.agg',a.fixtures/(name+'.agg'),shallow=False),name
    results.append(dict(name=name,status='PASS',aggregate_bytes_equal=True,**result))
    print(json.dumps(results[-1]),flush=True)
(a.log_dir/'battery.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS 7/7 publications: exact endpoint and aggregate bytes, no XSA environment')
