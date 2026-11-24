"""Score an actual-width 3 GB product, separately from its SHA verdict.
Same raw-to-packed stages, row oracle, CPU quotas and ~same RAM per input byte
as the 1 GB raw gate. Different text and matching frozen v8 finish binary.
"""
import json
import sys
from pathlib import Path
from overlap_resident_gate import ONE, THREE

def read(path,name): return json.loads((path/name).read_text())
def binaries(arm):
    return {Path(p).name:h for h,p in (l.split('  ',1) for l in (arm/'binaries.sha').read_text().splitlines() if '  /' in l)}

def main(d):
    base=Path(__file__).resolve().parent/'receipts/packed-raw-1g'
    for arm,cfg in ((base,ONE),(d,THREE)):
        ident=read(arm,'identity.json'); b=read(arm,'build.json'); o=read(arm,'oracle.json'); f=read(arm,'front-end.json')
        assert ident['verified'] and ident['sha256']==cfg.sha and len(ident['members'])==9
        assert b['rc']=={'resident':0,'sink':0,'finish':0} and b['packed_tape_retired'] and not b['stop'] and o['rc']==f['rc']==0
        assert b['raw_counted_highwater']<=2*cfg.n and read(arm,'scratch-plan.json')['counted_bound']<=2*cfg.n
        for cg,cap,cpu in [(b['shared_after'],cfg.shared_gib<<30,'400000 100000'),(o['cgroup'],cfg.oracle_mib<<20,'100000 100000')]:
            assert int(cg['memory.max'])==cap and cg['cpu.max']==cpu and cg['memory.swap.max']=='0' and 'oom_kill 0' in cg['memory.events'].splitlines()
        assert f'ORACLE rows={cfg.n+1} independent_final_rank={cfg.n+1} all_adjacent_sa_lcp_bwt_verified=1' in (arm/'oracle.log').read_text()
    log=(d/'resident.log').read_text(); assert 'native_above_i32=true' in log and f"RESIDENT_CHECK recurrence_ranks={f['m']} virtual_end=0" in log
    assert f['D']>=2**31 and max(f['array_bounds'].values())<=THREE.arrays_gib<<30
    ref=read(base,'build.json'); ratios={name:(b[name]/THREE.n)/(ref[name]/ONE.n) for name in ('raw_to_product_s','wall_s')}
    rr=sum(x['read_bytes'] for x in b['process'].values()); ww=sum(x['write_bytes'] for x in b['process'].values())
    # One cold pass worth of input to prepared stage (words, IDs, finish corpus).
    cold=sum(f['columns'].values())+THREE.n
    # Cross-rung normalized IO target, not a same-size disk/volatile A/B.
    disk=read(base.parent/'resident-wide-1g','build.json'); wide_w=sum(x['write_bytes'] for x in disk['process'].values())
    wr=(ww/THREE.n)/(wide_w/ONE.n);io_ok=rr<=1.05*cold and wr<=0.5
    result=dict(relative_to='verified 1 GB raw packed product; normalized by input bytes',normalized_wall=ratios,
                target_under_2x=max(ratios.values())<2,promotion_under_3x=max(ratios.values())<3,
                prepared_attributed_read_bytes=rr,cold_input_bytes=cold,prepared_writes_vs_disk_control_per_byte=wr,normalized_io_pass=io_ok)
    if b['profile']=='packed-3g-bitmap':
        control=base.parent/'packed-native-3g'; cb=read(control,'build.json'); cf=read(control,'front-end.json')
        assert cb['profile']=='packed-3g' and read(control,'identity.json')==ident
        for key in ('D','columns','m','k','descriptors','array_bounds'):assert f[key]==cf[key]
        for k,v in binaries(control).items(): assert binaries(d).get(k)==v,k
        bm=b['bitmap_placement']; expected=(THREE.n+1+7)//8
        assert bm==dict(bytes_reserved=expected,sampled_bytes=expected,events=['GATE_BITMAP create=tmpfs exclusive=1','GATE_BITMAP unlink=tmpfs'],empty_after_finish=True),bm
        assert b['individual_sampled_peaks'].get('frag.chi-bitmap',0)==0 and cb['individual_sampled_peaks']['frag.chi-bitmap']==expected
        assert b['volatile_handoff']['reserved_bytes']==cb['volatile_handoff']['reserved_bytes']+expected
        assert read(d,'scratch-plan.json')==read(control,'scratch-plan.json')
        ratios_io={k:sum(p[k] for p in b['process'].values())/sum(p[k] for p in cb['process'].values()) for k in ('read_bytes','write_bytes')}
        raw_ratios={k:v/cb['raw_worker_io'][k] for k,v in b['raw_worker_io'].items()}
        wall={k:b[k]/cb[k] for k in ('wall_s','raw_to_product_s')}
        matched=ratios_io['read_bytes']<=1.05 and ratios_io['write_bytes']<=0.50 and max(wall.values())<3
        result.update(matched_prepared_io=ratios_io,matched_raw_io=raw_ratios,matched_wall=wall,matched_bitmap_pass=matched)
        io_ok=io_ok and matched
    (d/'goals.json').write_text(json.dumps(result,indent=2)+'\n');print('NATIVE_3G_SCORE',result,flush=True)
    if not(io_ok and result['promotion_under_3x']):raise SystemExit(75)

if __name__=='__main__':main(Path(sys.argv[1]))
