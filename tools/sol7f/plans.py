#!/usr/bin/env python3
"""Front-door --plan-only projections; sparse inputs and explicit df scenarios.

This never constructs corpus data. The df fixture changes only the capacity
survey of this child process; it is not an assertion about installed hardware.
"""
import json, os, subprocess, sys
from pathlib import Path
binary=Path(sys.argv[1]).resolve()
root=Path('/mnt/nvme2n1/erikg/sol7f-plans');root.mkdir(exist_ok=True)
binpath=root/'survey-bin';binpath.mkdir(exist_ok=True)
df=binpath/'df'
df.write_text('#!/usr/bin/env python3\nimport os\nprint("Avail Use%")\nprint(os.environ["SOL7F_SURVEY_FREE"]+" 10%")\n');df.chmod(0o755)
results=[]
for label,n,free,chunks in [('100gb',100_000_000_000,2_600_000_000_000,100),('pile',1_310_000_000_000,13_500_000_000_000,1310)]:
    corpus=root/(label+'.sparse')
    with corpus.open('wb') as f:f.truncate(n)
    env=dict(os.environ,PATH=str(binpath)+os.pathsep+os.environ['PATH'],SOL7F_SURVEY_FREE=str(free),CROSS_V2_PRODUCERS='8',CROSS_V2_SPOOL_BYTES=str(2*n))
    cmd=[str(binary),'build','--input',str(corpus),'--scratch',str(root/(label+'-scratch')),'--v2-pipeline','--plan-only','--chunks',str(chunks),'--kway',str(chunks),'--threads','96','--memory-gb','100']
    r=subprocess.run(cmd,env=env,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    Path('tools/sol7f/'+label+'-plan.txt').write_text(r.stdout+r.stderr)
    assert r.returncode==0,(r.returncode,r.stderr)
    assert 'FEASIBLE: ALL-PHASE' in r.stdout and 'INFEASIBLE' not in r.stdout,r.stdout
    results.append(dict(label=label,n=n,scenario_free_bytes=free,spool_budget_bytes=2*n,command=cmd,rc=r.returncode))
Path('tools/sol7f/plans.json').write_text(json.dumps(results,indent=2)+'\n')
print('SOL7F_PLAN_ONLY_PASS cases=2 configured_spool=2n capacity_scenarios=2.6TB,13.5TB sparse_inputs=1')
