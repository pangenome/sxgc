"""No payload: validate admission bounds and that a parallel miss never becomes an identity failure or win."""
from pathlib import Path
import tempfile
import overlap_finite_emit64_study as g
from overlap_finite_route64_plan import bands,CELLS

def rejects(f):
    try:f()
    except (AssertionError,ValueError,RuntimeError):return
    raise AssertionError('accepted invalid condition')

def run():
    for delta in (0,899,900,1501):rejects(lambda:g.window(1000,1000+delta))
    for delta in (901,1030,1500):g.window(1000,1000+delta)
    assert g.OTHER==29558552 and g.SLOTS==192001 and g.BOUND==196914175<2*g.N and g.FILE_UP==167355623 and set(g.sources())=={'raw','parse.u32','old-inverse'}
    rows=[0]*CELLS
    for i in range(129):rows[i*500]=i%7+1
    for cap in (1,3,18,99):assert len(bands(rows,cap))<=2*g.ceil(sum(rows),cap)+1
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);save=lambda k,v:g.s.save(d/(k+'.json'),v);hashes={k:h for k,(_,_,h) in g.sources().items()};rev=dict(start_unix=1000,deadline=2200,prior=g.PREV_SHA,slots=g.SLOTS,row_target=g.TARGET,counted_upper=g.BOUND,cores=list(range(5)),source_hashes=hashes);save('admission',rev)
        def grant(mem,cpu):return {'memory.max':str(mem),'memory.peak':'10000','memory.swap.max':'0','cpu.max':f'{100000*cpu} 100000','memory.events':'oom 0\noom_kill 0'}
        save('setup',dict(source_hashes=hashes,rank_sha256='e'*64,after=grant(192<<20,1),private_stats={k:[0] for k in ('raw','oracle-raw','inverse.u64')}));(d/'rank-proof.log').write_text(f'VERIFIED_PARSE64 rows={g.M} source_token_width=4 ranks=uint64 permutation=1 strict_recurrence=1 virtual_end_rank=0\n');base=dict(n=g.N,m=g.M,k=g.K,descriptors=g.U,rows=g.N+1,max_tie=6545,row_bytes=12*(g.N+1),decoded=g.U,populated=900,wire=1000,counted=g.OTHER+1000,arrays=1<<20,leaves=6,bands=6,groups=4300,exact_bypass=0,bitmap_groups=3,max_mass=50000,local_scans=0)
        for w in (4,1):
            save(f'cold{w}',dict(resident_pages=0,inputs=['raw','inverse.u64','oracle-raw'],wall_s=.1));save(f'pair{w}',dict(emitter=0,oracle=0,pipe_bytes=65536,prepared_to_verified_EOF_s=50 if w==4 else 100));(d/f'oracle{w}.log').write_text(f'VERIFIED rows={g.N+1} final_rank={g.N+1} narrow_sha256={g.old.DIGEST}\n');(d/f'emit{w}.log').write_text('FINITE_EMIT '+' '.join(f'{k}={v}' for k,v in (base|dict(team=w)).items())+'\n')
            for role in ('emit','oracle'):
                prod=role=='emit';save(f'{role}{w}',dict(after=grant(g.RAM if prod else g.ORACLE,4 if prod else 1),process=dict(read_bytes=100,write_bytes=100,cpu_s=2),affinity=rev['cores'][:4] if prod else [4],rc=0,status='COMPLETED',stop=None,counted_sampled=500,allocated_including_raw_copies=1000,sampled_rss=9999,per_device_io=None,wall_s=50,io_caps=[12*g.N,g.BOUND+(8<<20)] if prod else [2*g.N,4<<20]))
        assert g.score(d)['parallel_goal_met'];pair=g.load(d,'pair4.json');pair['prepared_to_verified_EOF_s']=81;save('pair4',pair);assert not g.score(d)['parallel_goal_met'];(d/'oracle1.log').write_text('incomplete');rejects(lambda:g.score(d));print('PASS matched stream controls, timing miss preserved, no promotion on missing raw-row witness, fixed source and scratch proofs')
if __name__=='__main__':run()
