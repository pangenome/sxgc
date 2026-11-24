"""Capped prepared-column producer experiment. Output is hashed, not staged.
A separate read-only raw oracle certifies the reference digest in the verify arm.
"""
import os
from pathlib import Path
import re
import subprocess as sp
import sys
import time
import json
from overlap_resident_gate import CG,cg_info,proc_info,finished,save

N=100_000_503; GRANT=(14<<30)//10; BLOCK=65536*12

def build(d,exe,lanes,audited):
    limits=cg_info();assert limits['cpu.max']=='400000 100000' and limits['memory.max']==str(2<<30) and limits['memory.swap.max']=='0'
    src=d/'in';flag=d/'stop.requested';assert (src/'corpus').stat().st_size==N
    queue=2*lanes*BLOCK if lanes>1 else 0
    scratch=sum((src/x).stat().st_size for x in ('dictionary.refs','parse.u32'))+queue+3*BLOCK+(2<<20);assert scratch<=2*N
    env={k:v for k,v in os.environ.items() if not k.startswith('XSA_')};env.update(XSA_RESIDENT_WIDE_DICT='1',XSA_RESIDENT_RADIX_LANES=str(lanes),XSA_RESIDENT_RADIX_STEP='16384')
    fds=(os.dup(1),) if audited else ();start=time.monotonic()
    if fds:env['XSA_RESIDENT_AUDIT_FD']=str(fds[0])
    with (d/'resident.log').open('xb') as log,(d/'digest.log').open('xb') as digest:
        p=sp.Popen([exe,src,flag,str(GRANT)],stdout=sp.PIPE,stderr=log,env=env,pass_fds=fds)
        for fd in fds:os.close(fd)
        sink=sp.Popen(['sha256sum'],stdin=p.stdout,stdout=digest);p.stdout.close();stats={};rc={}
        workers={'producer':p,'hash':sink};of={n:open(f'/proc/{w.pid}/io') for n,w in workers.items()}
        while len(rc)<len(workers):
            for n,w in workers.items():
                if n in rc:continue
                done=finished(w);stats[n]=proc_info(w.pid,of[n]);
                if done:rc[n]=w.wait();of[n].close()
            if time.monotonic()-start>300 or sum(v['read_bytes'] for v in stats.values())>15*N:flag.touch(exist_ok=True)
            if len(rc)<len(workers):time.sleep(.1)
    b=dict(lanes=lanes,verify=audited,process=stats,rc=rc,wall_s=time.monotonic()-start,cgroup=cg_info(),array_grant=GRANT,queue_reserved=queue,counted_scratch_bound=scratch,stop=flag.exists());save(d/'metrics.json',b)
    assert rc=={'producer':0,'hash':0} and not b['stop'] and 'oom 0' in b['cgroup']['memory.events'].splitlines()
    log=(d/'resident.log').read_text(); fields=lambda tag:dict(re.findall(r'(\w+)=(\S+)',next(l for l in log.splitlines() if l.startswith(tag))))
    b.update(digest=(d/'digest.log').read_text().split()[0],emit_s=float(fields('RESIDENT_PHASE phase=emit ')['seconds']),stream=fields('RESIDENT_STREAM'))
    assert int(b['stream']['rows'])==N+1
    if lanes>1:
        b.update(plan=fields('RESIDENT_RADIX_PLAN'),join=fields('RESIDENT_RADIX_JOIN'));b['join']['lane_rows']=json.loads(re.search(r'lane_rows=(\[[^\]]+\])',log)[1]);assert sum(b['join']['lane_rows'])==N
        assert int(b['plan']['row_queue_reserved'])==queue and b['join']['serial_row_comparisons']=='0'
    save(d/'metrics.json',b);print('RADIX_ARM',d.name,lanes,'emit_s',round(b['emit_s'],3),'wall_s',round(b['wall_s'],3),file=sys.stderr,flush=True)

def score(root):
    get=lambda name:json.loads((root/name/'metrics.json').read_text());v=get('verify');assert f'ORACLE rows={N+1} independent_final_rank={N+1} all_adjacent_sa_lcp_bwt_verified=1' in (root/'verify/oracle.log').read_text()
    arms={a:get(a) for a in ('one','four','two','repeat-one')}
    for a in arms.values():
        assert not a['verify'] and a['digest']==v['digest']
        for k in ('rows','descriptors','groups','max_group'):assert a['stream'][k]==v['stream'][k]
    baseline=(arms['one']['emit_s']+arms['repeat-one']['emit_s'])/2
    out=dict(rows=N+1,digest=v['digest'],scope='prepared 100 MB resident producer to hash sink; both serial endpoints reported, no full finish',serial_emit_s=[arms[a]['emit_s'] for a in ('one','repeat-one')],
             emit_s={a:b['emit_s'] for a,b in arms.items()},emit_speedup_vs_serial_mean={a:baseline/arms[a]['emit_s'] for a in ('two','four')},wall_s={a:b['wall_s'] for a,b in arms.items()});save(root/'result.json',out);print('RADIX_RESULT',out,flush=True)

if __name__=='__main__':
    if sys.argv[1]=='score':score(Path(sys.argv[2]))
    else:build(Path(sys.argv[2]),Path(sys.argv[3]),int(sys.argv[4]),sys.argv[1]=='verify')
