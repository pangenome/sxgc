#!/usr/bin/env python3
import json, os, subprocess
from pathlib import Path
root=Path('/mnt/nvme2n1/erikg/sol7e-freshstart')
checks=[]
def check(name,cmd,extra=None):
    with (root/(name+'.log')).open('w') as log:
        r=subprocess.run(cmd,env=dict(os.environ,**(extra or {})),stdout=log,stderr=subprocess.STDOUT)
    lines=(root/(name+'.log')).read_text().splitlines()
    checks.append(dict(command=cmd,result='passed' if r.returncode==0 else 'failed',output=[s for s in lines if 'PASS' in s][-4:],log=str(root/(name+'.log'))))
    (root/'validation.json').write_text(json.dumps(checks,indent=2)+'\n')
    assert r.returncode==0,(name,r.returncode)
merge=str(root/'cross_lcp_merge')
for p,k in [(1,1),(4,2),(8,8),(16,8)]:
    check(f'selftest-p{p}-k{k}',[merge,'--selftest','200','--seed','1','--kway',str(k),'--threads','16'],{'CROSS_SHARDS':str(p),'CROSS_V2_PRODUCERS':str(p)})
check('build-runtime',['c++','-O3','-std=c++17','-pthread','xsa/runtime/bit6/cross_lcp_merge.cpp','-latomic','-o',str(root/'runtime-cross_lcp_merge')])
check('runtime-selftest',[str(root/'runtime-cross_lcp_merge'),'--selftest','20','--seed','1','--kway','8','--threads','16'],{'CROSS_V2_PRODUCERS':'16','CROSS_SHARDS':'16'})
check('build-witness',['c++','-O3','-std=c++17','-pthread','tools/sol7d/seam_witness.cpp','-latomic','-o',str(root/'seam_witness')])
check('counter-summary',['python3','tools/sol7e/work_witness.py',str(root/'seam_witness'),str(root)])
check('legacy-seed-witness',['python3','tools/sol7d/work_witness.py',str(root/'seam_witness'),str(root/'seed-regression')])
check('segment-model',['python3','tools/sol7e/segment_model.py'])
check('threaded-model',['python3','tools/sol7b/threaded_model.py'])
check('wait-model',['python3','tools/sol5i/wait_model.py'])
check('build-flow',['c++','-O3','-std=c++17','-pthread','tools/sol5i/flow_oracle.cpp','-latomic','-o',str(root/'flow_oracle')])
check('flow',[str(root/'flow_oracle')])
check('build-finish',['c++','-O3','-std=c++17','-pthread','-I/home/erikg/usr/include','bit6/pile_finish.cpp','-L/home/erikg/usr/lib','-lsdsl','-ldivsufsort','-ldivsufsort64','-latomic','-o',str(root/'pile_finish')])
for name,args in [('sweep',['--selftest-sweep-monoid']),('repair',['--selftest-repair-chunks']),('seam-spool',['--selftest-seam-spool',str(root/'seam-spool')])]:
    check(name,[str(root/'pile_finish')]+args)
