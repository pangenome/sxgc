#!/usr/bin/env python3
"""Complete slice frontends and byte gates. Baselines read independent copies."""
from run_gate import HERE, WORK, BIN, record, run
import pathlib, subprocess, sys, shutil, struct
size=int(sys.argv[1]) if len(sys.argv)>1 else 100_000_000
for w,p in [(10,100),(5,100),(20,100),(3,5)]:
    tag=f'parse-{size}-w{w}-p{p}';prefix=WORK/tag
    if not pathlib.Path(str(prefix)+'.parse').exists():continue
    run(tag+'-l2',[WORK/'pfp-probe','-i',str(prefix)+'.parse','-w','5','-p','11'])
    ref=WORK/(tag+'-baseline');ref.mkdir(exist_ok=True)
    for ext in ['.dict','.parse','.parse.dict','.parse.parse']:
        subprocess.run(['cp','--reflink=auto',str(prefix)+ext,str(ref/'parse')+ext],check=True)
    run(tag+'-memory',[BIN,'--l1-prefix',prefix,'--w1',w,'--w2','5','--threads','16','--tmp-dir',WORK])
    run(tag+'-baseline',['/home/erikg/sxgc/sealed-tools/rpfbwt','--l1-prefix',ref/'parse','--w1',w,'--w2','5','--threads','16','--tmp-dir',ref])
    for ext in ['.rlebwt','.rlebwt.meta','.ssa','.ssa_t']:
        subprocess.run(['cmp',str(prefix)+ext,str(ref/'parse')+ext],check=True)
    n,r=struct.unpack('<QQ',pathlib.Path(str(prefix)+'.rlebwt.meta').read_bytes()[:16])
    record(stage=tag,status='GATE_PASS',n=n,r=r,R_over_n=r/n)
