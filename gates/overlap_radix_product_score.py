"""Same-size raw→v8 product A/B. Passes identity independently of speed.
Never promote a producer/hash-only result as a full product improvement.
"""
import json
import re
from pathlib import Path
import sys
from overlap_resident_gate import ONE,radix_lanes,save
from overlap_three_score import binaries


def read(d,name):return json.loads((d/name).read_text())
def metrics(d):
    b=read(d,'build.json');f=read(d,'front-end.json');o=read(d,'oracle.json');i=read(d,'identity.json');s=read(d,'scratch-plan.json');lanes=radix_lanes(b['profile']);q=lanes*2*65536*12 if lanes>1 else 0
    assert b['profile']==f'packed-radix-{lanes}' and i['verified'] and i['sha256']==ONE.sha and len(i['members'])==9
    assert b['rc']=={'resident':0,'sink':0,'finish':0} and not b['stop'] and b['packed_tape_retired'] and f['rc']==o['rc']==0
    assert f'ORACLE rows={ONE.n+1} independent_final_rank={ONE.n+1} all_adjacent_sa_lcp_bwt_verified=1' in (d/'oracle.log').read_text()
    for cg,cap,cpu in [(b['shared_after'],20<<30,4),(o['cgroup'],1400<<20,1)]:
        assert int(cg['memory.max'])==cap and int(cg['memory.peak'])<=cap and cg['cpu.max']==f'{cpu*100000} 100000' and cg['memory.swap.max']=='0' and 'oom 0' in cg['memory.events'].splitlines()
    assert s['parts']['row_queue']==b['individual_sampled_peaks'].get('row_queue_reserved',0)==q
    assert b['raw_counted_highwater']<=s['limit']==2*ONE.n and b['counted_sampled_peak']<=s['counted_bound']<=s['limit']
    assert b['bitmap_placement']==dict(bytes_reserved=(ONE.n+8)//8,sampled_bytes=(ONE.n+8)//8,empty_after_finish=True,events=['GATE_BITMAP create=tmpfs exclusive=1','GATE_BITMAP unlink=tmpfs'])
    log=(d/'resident.log').read_text();fields=lambda tag:dict(re.findall(r'(\w+)=(\S+)',next(l for l in log.splitlines() if l.startswith(tag))))
    assert f"RESIDENT_CHECK inverse_ranks={f['m']} virtual_end=0" in log
    assert int(fields('RESIDENT_PLAN')['emit_bound'])==f['array_bounds']['emit']<=14<<30
    stream=fields('RESIDENT_STREAM'); assert int(stream['rows'])==ONE.n+1
    core={k:int(stream[k]) for k in ('rows','descriptors','groups','max_group')}
    if q:
        plan=fields('RESIDENT_RADIX_PLAN');join=fields('RESIDENT_RADIX_JOIN');assert int(plan['row_queue_reserved'])==q and int(plan['lanes'])==lanes and join['serial_row_comparisons']=='0'
    else:assert 'RESIDENT_RADIX_JOIN' not in log
    return b,f,i,core,dict(raw=b['raw_to_product_s'],prepared=b['wall_s'],emit=float(fields('RESIDENT_PHASE phase=emit ')['seconds']),sink_writes=float(stream['sink_write_s']),audit_writes=float(stream['audit_write_s']))

def score(control,parallel,dest):
    cb,cf,ci,cc,ct=metrics(control);pb,pf,pi,pc,pt=metrics(parallel)
    assert cb['profile']=='packed-radix-1' and radix_lanes(pb['profile'])>1 and binaries(control)==binaries(parallel)
    assert ci==pi and cc==pc and cf['columns']==pf['columns'] and cf['D']==pf['D'] and cf['m']==pf['m'] and cf['k']==pf['k']
    factor={k:pt[k]/ct[k] for k in ('raw','prepared','emit')}; io={k:sum(w[k] for w in pb['process'].values())/sum(w[k] for w in cb['process'].values()) for k in ('read_bytes','write_bytes')}
    raw_io={k:pb['raw_worker_io'][k]/cb['raw_worker_io'][k] for k in io};ok=factor['emit']<=.90 and factor['raw']<=.95 and max(io.values())<=1.05
    report=dict(control=str(control),parallel=str(parallel),lanes=radix_lanes(pb['profile']),same_binary_and_product=True,serial_s=ct,parallel_s=pt,
                time_ratio=factor,worker_io_ratio=io,raw_io_ratio=raw_io,promotion_target='emit <=0.90x, raw wall <=0.95x, post-front worker reads/writes <=1.05x',matched_speed_io_pass=ok)
    save(dest,report);print('MATCHED_RADIX_PRODUCT',report,flush=True)
    if not ok:raise SystemExit(75)

if __name__=='__main__':score(*(Path(x) for x in sys.argv[1:]))
