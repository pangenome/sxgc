#!/usr/bin/env python3
import json,os,subprocess
from pathlib import Path
root=Path('/mnt/nvme2n1/erikg/sol8a-standing');root.mkdir(exist_ok=True)
producer='/mnt/nvme2n1/erikg/sol8a-measure/radix_sweep'
finish='/mnt/nvme2n1/erikg/sol7e-freshstart/pile_finish'
commands=[([producer,'--selftest','200','--seed','1','--kway',str(k),'--threads','16'],f'selftest-k{k}') for k in [1,2,8]]
commands += [([finish,'--selftest-sweep-monoid'],'sweep'),([finish,'--selftest-repair-chunks'],'repair'),([finish,'--selftest-seam-spool',str(root/'spool')],'spool'),(['/mnt/nvme2n1/erikg/sol7e-freshstart/flow_oracle'],'flow'),(['python3','tools/sol7f/segment_model.py'],'segment-model')]
results=[]
for command,name in commands:
    with (root/(name+'.log')).open('w') as log:r=subprocess.run(command,stdout=log,stderr=log,env=dict(os.environ,TMPDIR=str(root)))
    lines=[line for line in (root/(name+'.log')).read_text().splitlines() if 'PASS' in line or ('deadlocks' in line)]
    results.append(dict(command=command,result='passed' if r.returncode==0 else 'failed',returncode=r.returncode,output=lines[-10:]))
    print(name,r.returncode,flush=True)
    assert r.returncode==0
(root/'validation.json').write_text(json.dumps(results,indent=2)+'\n')
