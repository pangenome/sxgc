"""Certification is separate; score only a fully verified 10 GB raw build."""
import json
from pathlib import Path
import re
import sys
from overlap_resident_gate import ONE,THREE,TEN,save

def get(d,name):return json.loads((d/name).read_text())
def traffic(b,k):return sum(x[k] for x in b['process'].values())

def score(d):
    root=Path(__file__).resolve().parent/'receipts'; b=get(d,'build.json');f=get(d,'front-end.json');o=get(d,'oracle.json');ident=get(d,'identity.json');s=get(d,'scratch-plan.json')
    assert ident['verified'] and ident['sha256']==TEN.sha and ident['reference_table_identical'] and len(ident['members'])==9
    assert b['profile']=='packed-10g-bitmap' and b['rc']==dict(resident=0,sink=0,finish=0) and not b['stop'] and b['packed_tape_retired'] and f['rc']==o['rc']==0
    assert (d/'exits.log').read_text().strip()=='build=0 oracle=0'
    assert b['raw_counted_highwater']<=s['limit']==2*TEN.n and b['counted_sampled_peak']<=s['counted_bound']<=s['limit']
    for cg,cap,cpu in ((b['shared_after'],TEN.shared_gib<<30,'400000 100000'),(o['cgroup'],TEN.oracle_mib<<20,'100000 100000')):
        assert int(cg['memory.max'])==cap and int(cg['memory.peak'])<=cap and cg['cpu.max']==cpu and cg['memory.swap.max']=='0'
        assert 'oom 0' in cg['memory.events'].splitlines() and 'oom_kill 0' in cg['memory.events'].splitlines()
    expected=(TEN.n+8)//8;assert b['bitmap_placement']==dict(bytes_reserved=expected,sampled_bytes=expected,events=['GATE_BITMAP create=tmpfs exclusive=1','GATE_BITMAP unlink=tmpfs'],empty_after_finish=True)
    assert b['individual_sampled_peaks'].get('frag.chi-bitmap',0)==0 and b['volatile_handoff']['sampled_tmpfs_allocated']<=b['volatile_handoff']['reserved_bytes']
    ref=get(root/'packed-front-10g','front-end.json');assert f['columns']==ref['columns'] and f['array_bounds']==ref['proposed_array_bytes']
    assert f['D']>2**32 and max(f['array_bounds'].values())<=TEN.arrays_gib<<30
    log=(d/'resident.log').read_text();assert 'RESIDENT_TRANSPORT row43=true text_width=8 lcp_width=8 rank_width=4' in log and 'native_above_i32=true' in log
    assert f"RESIDENT_CHECK recurrence_ranks={f['m']} virtual_end=0" in log and f'RESIDENT_STREAM rows={TEN.n+1}' in log
    plan={s:int(v) for s,v in re.findall(r'(\w+)=(\d+)',next(l for l in log.splitlines() if l.startswith('RESIDENT_PLAN ')))}
    for name,key in [('parse','parse'),('sort','dict'),('emit','emit'),('aux','aux')]:assert plan[key+'_bound']==f['array_bounds'][name]
    assert f'ORACLE rows={TEN.n+1} independent_final_rank={TEN.n+1} all_adjacent_sa_lcp_bwt_verified=1 above_u32={TEN.n-0xffffffff} ' in (d/'oracle.log').read_text()
    ratios={}; raw_io={}; post_io={}
    for cfg,folder in [(ONE,'packed-raw-1g'),(THREE,'packed-3g-bitmap')]:
        assert get(root/folder,'identity.json')['sha256']==cfg.sha
        cb=get(root/folder,'build.json'); ratios[folder]={k:(b[k]/TEN.n)/(cb[k]/cfg.n) for k in ('wall_s','raw_to_product_s')}
        raw_io[folder]={k:(b['raw_worker_io'][k]/TEN.n)/(cb['raw_worker_io'][k]/cfg.n) for k in ('read_bytes','write_bytes')}
        post_io[folder]={k:(traffic(b,k)/TEN.n)/(traffic(cb,k)/cfg.n) for k in ('read_bytes','write_bytes')}
    high=max(max(pair.values()) for pair in ratios.values());cold=sum(f['columns'].values())+TEN.n;reads=traffic(b,'read_bytes');writes=traffic(b,'write_bytes')
    disk=get(root/'resident-wide-1g','build.json');wr=(writes/TEN.n)/(traffic(disk,'write_bytes')/ONE.n);ok=reads<=1.05*cold and wr<=0.50
    result=dict(relative_to='normalized cross-rung, not same-text A/B; v8 profile, rung-specific frozen finish/remap',normalized_wall=ratios,
                normalized_raw_io=raw_io,normalized_prepared_io=post_io,target_under_2x=high<2,promotion_under_3x=high<3,
                prepared_read_bytes=reads,cold_input_bytes=cold,prepared_write_bytes=writes,normalized_write_ratio_vs_wide_disk=wr,normalized_io_pass=ok)
    save(d/'goals.json',result); print('TEN_PRODUCT_SCORE',result,flush=True)
    if not(ok and high<3):raise SystemExit(75)
if __name__=='__main__':score(Path(sys.argv[1]))
