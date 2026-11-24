"""Certification is separate; score only a fully verified 10 GB raw build."""
import json
from pathlib import Path
import re
import sys
from overlap_resident_gate import ONE,THREE,TEN,TEN_ARMS,dict_extra,emit_extra,save

from overlap_three_score import binaries

# The one marker-less sink allowed in a historical comparison. Pin the pair,
# not merely the source label. Everything except these workers must match.
HISTORICAL='02da5debe029d7ac3b6e3c0b864a2f91893c0812'
HISTORICAL_WORKERS={'overlap_resident':'3248d396eea8041dc71c05a2028014f69bde4ca8b3c22ec5d2799fc9ff61c3a3',
                    'pfp_sort_r':'01036e2ed787553a55808c331dda8ac2848550cbbf7926e144deb1e2ed48ed25'}
FIXED={'monitor.so':'fe5173c7d32215bdc7d41e59fd70edf059621ce83b38fb87b2e4d73ebbcb960a',
       'bitmap.so':'ed0b651a8907e0be70ac9e029d1684c08997de633ae2cef6ed3844007c80bc1f',
       'lexparse':'fd17c0fa38da23f59738294a0272c0668af1f1a95ad9f26c83787e9a5b11c4cf',
       'oracle':'5231446d19b42c8f502cafd4a7a283f80171c24fdafd693bb98a9540aa579ddc',
       'xsa_finish_cur':'9a0bde6739ba1819f8d298beca2c8a9bf53480fab19f637b49cf6e3c8e200fa7',
       'remap':'0ce35ac950b471c0da0e9a7773a6d82871d53d5de5a496bb9ffda6078b896780'}

def get(d,name):return json.loads((d/name).read_text())
def traffic(b,k):return sum(x[k] for x in b['process'].values())

def verified_arm(d):
    root=Path(__file__).resolve().parent/'receipts'; b=get(d,'build.json');f=get(d,'front-end.json');o=get(d,'oracle.json');ident=get(d,'identity.json');s=get(d,'scratch-plan.json')
    assert ident['verified'] and ident['sha256']==TEN.sha and ident['reference_table_identical'] and len(ident['members'])==9
    matched=b['profile'] in TEN_ARMS;assert matched or b['profile']=='packed-10g-bitmap'
    lanes,batch,threads,reclaim=TEN_ARMS.get(b['profile'],(1,0,1,0))
    assert b['rc']==dict(resident=0,sink=0,finish=0) and not b['stop'] and b['packed_tape_retired'] and f['rc']==o['rc']==0
    assert (d/'exits.log').read_text().strip()=='build=0 oracle=0'
    assert b['raw_counted_highwater']<=s['limit']==2*TEN.n and b['counted_sampled_peak']<=s['counted_bound']<=s['limit']
    for cg,cap,cpu in ((b['shared_after'],TEN.shared_gib<<30,'400000 100000'),(o['cgroup'],TEN.oracle_mib<<20,'100000 100000')):
        assert int(cg['memory.max'])==cap and int(cg['memory.peak'])<=cap and cg['cpu.max']==cpu and cg['memory.swap.max']=='0'
        assert 'oom 0' in cg['memory.events'].splitlines() and 'oom_kill 0' in cg['memory.events'].splitlines()
    expected=(TEN.n+8)//8;assert b['bitmap_placement']==dict(bytes_reserved=expected,sampled_bytes=expected,events=['GATE_BITMAP create=tmpfs exclusive=1','GATE_BITMAP unlink=tmpfs'],empty_after_finish=True)
    assert b['individual_sampled_peaks'].get('frag.chi-bitmap',0)==0 and b['volatile_handoff']['sampled_tmpfs_allocated']<=b['volatile_handoff']['reserved_bytes']
    ref=get(root/'packed-front-10g','front-end.json');assert f['columns']==ref['columns']
    for k in ('D','n','m','k','descriptors'):assert f[k]==ref['census'][k]
    bounds=ref['proposed_array_bytes'].copy();bounds['sort']+=dict_extra(f['D'],threads);bounds['emit']+=emit_extra(f['D'],lanes)
    assert f['array_bounds']==bounds
    assert f['D']>2**32 and max(f['array_bounds'].values())<=TEN.arrays_gib<<30
    log=(d/'resident.log').read_text();assert 'RESIDENT_TRANSPORT row43=true text_width=8 lcp_width=8 rank_width=4' in log and 'native_above_i32=true' in log
    assert f"RESIDENT_CHECK recurrence_ranks={f['m']} virtual_end=0" in log and f'RESIDENT_STREAM rows={TEN.n+1}' in log
    plan={s:int(v) for s,v in re.findall(r'(\w+)=(\d+)',next(l for l in log.splitlines() if l.startswith('RESIDENT_PLAN ')))}
    for name,key in [('parse','parse'),('sort','dict'),('emit','emit'),('aux','aux')]:assert plan[key+'_bound']==f['array_bounds'][name]
    assert f'ORACLE rows={TEN.n+1} independent_final_rank={TEN.n+1} all_adjacent_sa_lcp_bwt_verified=1 above_u32={TEN.n-0xffffffff} ' in (d/'oracle.log').read_text()
    if matched:
        hashes=binaries(d);assert set(hashes)==FIXED.keys()|HISTORICAL_WORKERS.keys() and all(hashes[k]==v for k,v in FIXED.items())
        assert f"RESIDENT_STREAM rows={TEN.n+1} descriptors={f['descriptors']} " in log
        assert (d/'build-source.log').read_text().splitlines()[0]=='8513cccb7d0ade1f4917752e3e8be33c3f7e49e3'
        assert 'zero resident pages' in (d/'cold.log').read_text() and f['cold_barrier_s']>0
        assert (d/'seam-fixtures.log').read_text().startswith('PASS global seam LCP')
        assert b['sxls_batched']==bool(batch) and b['omp_settings']==dict(XSA_RESIDENT_DICT_THREADS=str(threads),OMP_NUM_THREADS=str(threads),OMP_THREAD_LIMIT=str(threads),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0')
        base=dict(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22',XSA_GATE_PACKED_10G='1',XSA_RESIDENT_RADIX_LANES=str(lanes),XSA_RESIDENT_RADIX_STEP='16384')
        sink_controls=dict(base,XSA_TEST_RESIDENT_TRIPLES='1',XSA_BATCH_SXLS_ESCAPES=str(batch))
        if reclaim:sink_controls['XSA_PRIVATE_SXLS_RECLAIM']='1'
        assert b['worker_controls']==dict(resident=dict(base,**b['omp_settings'],XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1'),sink=sink_controls,finish=base)
        sink=d/'sink.log' if (d/'sink.log').exists() else d/'sink-metrics.log'
        lines=sink.read_text().splitlines();assert [x for x in lines if x.startswith('SS_PRIVATE_SXLS_RECLAIM ')]==(['SS_PRIVATE_SXLS_RECLAIM window=64 resume=unsupported'] if reclaim else [])
        marks=b['sxls_markers'];expected_markers=128 if reclaim else 2*((TEN.n+1+262143)//262144)
        assert marks['bands_on_tmpfs'] and marks['reclaim']==bool(reclaim) and marks['exact_final']==expected_markers and marks['sampled_peak']==marks['exact_final']
        assert all(b['individual_sampled_peaks'].get(k,0)>0 for k in ('ram_slices','ram_pieces'))
        assert not any(b['individual_sampled_peaks'].get(k,0) for k in ('slices','pieces','frag.chi-bitmap'))
        assert [x for x in lines if x.startswith('SS_SXLS_WRITER ')]==[f'SS_SXLS_WRITER batched_escapes={str(bool(batch)).lower()}']
        assert f"RESIDENT_DICT_SORT kernel=native_wide wide=true selected_threads={threads} fs=0 thread_workspace_bytes={dict_extra(f['D'],threads)}" in log.splitlines()
        queue=lanes*2*65536*12 if lanes>1 else 0
        assert s['parts']['row_queue']==b['individual_sampled_peaks'].get('row_queue_reserved',0)==queue
        rows=TEN.n+1;pieces=104*(44+5*65536+16*((rows.bit_length()*65536+63)//64)+40)
        table=ident['members'];parts=dict(retained=f['columns']['parse.u32'],pfck=64+8*(rows//22+2),bitmap=expected,sparse=table['11'][2],lcp_runs=table['14'][2],ef_spools=table['5'][2],pieces=pieces,slices=64*(48+17*262144),control_margin=8<<20,row_queue=queue)
        assert s['parts']==parts and b['volatile_handoff']['reserved_bytes']==sum(parts[k] for k in ('bitmap','pieces','slices','control_margin'))
        assert s['before_emit_bound']==sum(f['columns'].values())+(8<<20)+queue
        assert s['finish_bound']==sum(s['parts'].values()) and s['counted_bound']==max(s['finish_bound'],s['before_emit_bound'])
        assert plan['array_grant']==TEN.arrays_gib<<30
        if lanes==1:assert 'RESIDENT_RADIX_PLAN' not in log and 'RESIDENT_RADIX_JOIN' not in log
        else:
            def record(tag):
                line,=[x for x in log.splitlines() if x.startswith(tag+' ')];return line,{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)}
            _,r=record('RESIDENT_RADIX_PLAN');join,j=record('RESIDENT_RADIX_JOIN')
            assert r['lanes']==j['lanes']==lanes and r['sa_ranks_per_tile']==16384 and r['block_bytes']==65536*12 and r['row_queue_reserved']==queue
            assert r['raw_tiles']==(f['D']+16383)//16384==r['ranges']+r['empty']+r['absorbed']
            assert 0<r['ranges']==j['ranges']<=j['blocks'] and r['adjusted']<=r['ranges'] and r['heap_bound']<=40*f['k']+(1<<20)
            rows=json.loads(re.search(r'lane_rows=(\[[^\]]*\])',join)[1]);assert len(rows)==lanes and sum(rows)==TEN.n and j['serial_row_comparisons']==0
    return b,f,ident

def score(d):
    root=Path(__file__).resolve().parent/'receipts'; b,f,_=verified_arm(d)
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
    if b['profile'] not in TEN_ARMS and not(ok and high<3):raise SystemExit(75)
if __name__=='__main__':score(Path(sys.argv[1]))
