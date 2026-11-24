"""Product-level performance gate on the same prepared rung, not a raw-input or Pile projection.
Identity is recorded independently: a correct but slow result is still a speed refusal.
"""
import json
import re
import sys
from pathlib import Path

BASE = Path(__file__).resolve().parent/'receipts/resident-1g'

def read(path):
    return json.loads(path.read_text())

def verified(path):
    identity=read(path/'identity.json'); build=read(path/'build.json'); oracle=read(path/'oracle.json')
    assert identity['verified'] and not build['stop'] and build['rc']=={'sink':0,'finish':0,'resident':0} and oracle['rc']==0
    log=(path/'resident.log').read_text()
    rows=int(re.search(r'^RESIDENT_STREAM rows=(\d+) ',log,re.M)[1]); n=rows-1
    assert f'ORACLE rows={rows} independent_final_rank={rows} all_adjacent_sa_lcp_bwt_verified=1' in (path/'oracle.log').read_text()
    assert re.search(r'^RESIDENT_CHECK inverse_ranks=\d+ virtual_end=0$',log,re.M)
    return identity,build,oracle,n

def main(candidate):
    i,b,o,n=verified(candidate); ref,baseline,ro,bn=verified(BASE)
    assert n==bn==1_000_000_665 and i==ref and b['profile']=='wide'
    assert 'RESIDENT_INDEX wide=true' in (candidate/'resident.log').read_text()
    for meter,refmeter in [(b['shared_after'],baseline['shared_after']),(o['cgroup'],ro['cgroup'])]:
        assert all(meter[k]==refmeter[k] for k in ('memory.max','memory.swap.max','cpu.max'))
        assert meter['memory.swap.max']=='0' and 'oom_kill 0' in meter['memory.events'].splitlines()
    reserve=read(candidate/'scratch-plan.json')['counted_bound']; assert b['counted_sampled_peak']<=reserve<=2*n
    ratio=(b['wall_s']/n)/(baseline['wall_s']/bn); passed=ratio<10.0
    data=dict(phase='prepared IDs/refs to completed container including pipe audit',input_bytes=n,
              reference_commit='143ade3253f95984ca48069b2d772abfa5254bf6',reference_wall_s=baseline['wall_s'],wall_s=b['wall_s'],
              strict_limit_s=n/bn*baseline['wall_s']*10,slowdown=ratio,pass_strictly_under_10x=passed)
    (candidate/'speed.json').write_text(json.dumps(data,indent=2)+'\n')
    def traffic(build,key): return sum(worker[key] for worker in build['process'].values())
    goals=dict(target_under_2x=ratio<2, promotion_under_3x=ratio<3,
               target_s=2*baseline['wall_s'],promotion_limit_s=3*baseline['wall_s'],
               attributed_reads_ratio=traffic(b,'read_bytes')/traffic(baseline,'read_bytes'),
               attributed_writes_ratio=traffic(b,'write_bytes')/traffic(baseline,'write_bytes'),
               note='Traffic ratios exclude verifier; not seek counts or proof of a disk improvement.')
    (candidate/'goals.json').write_text(json.dumps(goals,indent=2)+'\n')
    print(f"SPEED {'PASS' if ratio<3 else 'REFUSED'} wall_s={b['wall_s']:.3f} normalized_slowdown={ratio:.4f} target=<2 promotion=<3")
    if not goals['promotion_under_3x']: raise SystemExit(75)

if __name__=='__main__': main(Path(sys.argv[1]))
