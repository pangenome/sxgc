"""Read-only source pin from 3 distinct raw copies; no eviction or producer launch.
Run only in a dedicated 192 MiB, one CPU, no-swap unit in an idle timing slot.
`--metadata-only` checks the receipt chain, not raw identity or launch authority.
"""
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import time
from overlap_bounded_component import S,SOURCE,N,DIGEST,BANK,check
from overlap_defaults import timing_lease
from overlap_published import published_main
from overlap_resident_gate import CG,REPO,cg_info,save
from overlap_scanplan_gate import worker_io

HIST=S/'radix-emit-100m-YQ7txA';REF=BANK/'radix-emit-100m';CUTOFF=1795531169

def evidence():
    hashes={}
    for k in ('result.json','verify/metrics.json','verify/oracle.log','verify/digest.log'):
        for p in (HIST/k,REF/k):check(p.is_file() and p.stat().st_size<1<<20 and not p.is_symlink(),'missing/oversize receipt')
        data=(REF/k).read_bytes();check((HIST/k).read_bytes()==data,'archived receipt drift: '+k);hashes[k]=hashlib.sha256(data).hexdigest()
    result=json.loads((REF/'result.json').read_text());verify=json.loads((REF/'verify/metrics.json').read_text())
    check(result['rows']==N+1==int(verify['stream']['rows']) and result['digest']==DIGEST==verify['digest'] and
          verify['rc']=={'producer':0,'hash':0} and verify['verify'] is True and verify['stop'] is False and
          (REF/'verify/digest.log').read_text().split()[0]==DIGEST and
          (REF/'verify/oracle.log').read_text().strip()==f'ORACLE rows={N+1} independent_final_rank={N+1} all_adjacent_sa_lcp_bwt_verified=1','invalid verified row source')
    return hashes

def run(deadline):
    check(time.time()+15<deadline<=CUTOFF,'insufficient time/outside study');before=cg_info()
    check((CG/'cgroup.procs').read_text().split()==[str(os.getpid())] and before['memory.max']==str(192<<20) and before['cpu.max']=='100000 100000' and before['memory.swap.max']=='0','not dedicated source-pin grant')
    with timing_lease():
        rev=published_main(REPO);hashes=evidence()
        d=Path(tempfile.mkdtemp(prefix='bounded-source-pin-',dir=S));print('source-pin receipt='+str(d),flush=True)
        sources={};seen=set();io0=worker_io();start=time.monotonic();report=dict(status='REFUSED',published_main=rev,cgroup_before=before,verified_receipt_sha256=hashes)
        try:
            for path in [SOURCE,HIST/'verify/in/corpus',HIST/'verify/oracle-corpus']:
                check(path.resolve()==path,'shared/symlinked path');h=hashlib.sha256()
                with open(path,'rb',opener=lambda p,f:os.open(p,f|os.O_NOFOLLOW)) as src:
                    st=os.fstat(src.fileno());key=(st.st_dev,st.st_ino);check(key not in seen and stat.S_ISREG(st.st_mode) and st.st_size==N and st.st_nlink==1,'not separate regular source copies');seen.add(key)
                    left=N
                    while left:
                        check(time.time()<deadline and not (d/'stop.requested').exists(),'time/stop');block=src.read(min(left,1<<20));check(block,'short copy');left-=len(block);h.update(block)
                    check(not src.read(1) and st.st_ctime_ns==path.stat().st_ctime_ns,'unstable copy')
                sources[str(path)]=dict(sha256=h.hexdigest(),dev_ino=key,bytes=N,mtime_ns=st.st_mtime_ns)
            raw={v['sha256'] for v in sources.values()};after=cg_info();check(len(raw)==1 and {'oom 0','oom_kill 0'}<=set(after['memory.events'].splitlines()),'raw pin mismatch or OOM')
            io={k:v-io0[k] for k,v in worker_io().items()};check(0<=io['read_bytes']<=4*N and io['write_bytes']<=1<<20,'metadata/hash IO cap')
            save(d/'source-pin.json',dict(source=str(SOURCE),n=N,raw_sha256=raw.pop(),narrow_row_sha256=DIGEST))
            report.update(status='SOURCE_PIN_VERIFIED',sources=sources,logical_read_bytes=3*N,worker_io=io,cgroup_after=after,wall_s=time.monotonic()-start)
        finally:save(d/'provenance.json',report)

if __name__=='__main__':
    if sys.argv[1:]==['--metadata-only']:print('RECEIPTS_ONLY',evidence())
    elif len(sys.argv)==2:run(int(sys.argv[1]))
    else:raise SystemExit('expected --metadata-only or deadline')
