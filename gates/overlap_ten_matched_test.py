"""Small receipt/config fixtures only. No corpus, systemd unit or timing run."""
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

from overlap_resident_gate import TEN,TEN_ARMS,config,dict_extra,emit_extra,expired,final_markers,omp_settings,save
from overlap_ten_score import get,verified_arm
from overlap_ten_matched_score import HISTORICAL_WORKERS,FIXED,HISTORICAL,compare,device_bytes

ROOT=Path(__file__).parent/'receipts'
PIN='8513cccb7d0ade1f4917752e3e8be33c3f7e49e3'

def fixture(d,profile):
    d.mkdir();lanes,batch,t,r=TEN_ARMS.get(profile,(1,0,1,0));q=lanes*2*65536*12 if lanes>1 else 0
    ref=get(ROOT/'packed-front-10g','front-end.json');f=ref['census'].copy();D=f['D'];bounds=ref['proposed_array_bytes'];bounds['sort']+=dict_extra(D,t);bounds['emit']+=emit_extra(D,lanes)
    proc=dict(cpu_s=12,read_bytes=3,write_bytes=4,syscw=5);f.update(columns=ref['columns'],array_bounds=bounds,rc=0,cold_barrier_s=1,wall_s=10,process=proc);save(d/'front-end.json',f)
    cg={'memory.max':200<<30,'memory.swap.max':'0','memory.peak':123,'cpu.max':'400000 100000','memory.events':'oom 0\noom_kill 0'}
    save(d/'oracle.json',dict(rc=0,cgroup={**cg,'memory.max':14000<<20,'cpu.max':'100000 100000'},process=proc))
    save(d/'identity.json',dict(verified=True,sha256=TEN.sha,reference_table_identical=True,members={str(i):[1,64,24,10] for i in (1,4,5,7,10,11,12,13,14)}))
    p=(TEN.n+8)//8;before=sum(f['columns'].values())+(8<<20)+q
    rows=TEN.n+1;pieces=104*(44+5*65536+16*((rows.bit_length()*65536+63)//64)+40);slices=64*(48+17*262144)
    parts=dict(bitmap=p,row_queue=q,pieces=pieces,slices=slices,control_margin=8<<20,retained=f['columns']['parse.u32'],pfck=64+8*(rows//22+2),sparse=24,lcp_runs=24,ef_spools=24)
    save(d/'scratch-plan.json',dict(parts=parts,finish_bound=sum(parts.values()),before_emit_bound=before,counted_bound=before,limit=2*TEN.n))
    b=dict(profile=profile,rc=dict(resident=0,sink=0,finish=0),stop=False,packed_tape_retired=True,raw_counted_highwater=12,counted_sampled_peak=q,shared_after=cg,
           individual_sampled_peaks=dict(row_queue_reserved=q,ram_slices=128,ram_pieces=128),bitmap_placement=dict(bytes_reserved=p,sampled_bytes=p,events=['GATE_BITMAP create=tmpfs exclusive=1','GATE_BITMAP unlink=tmpfs'],empty_after_finish=True),
           volatile_handoff=dict(sampled_tmpfs_allocated=1,reserved_bytes=p+pieces+slices+(8<<20)),sxls_batched=bool(batch),raw_worker_io=dict(read_bytes=12,write_bytes=16),process={k:proc for k in ('resident','sink','finish')},wall_s=90,raw_to_product_s=100,
           cpu_affinity=[0,1,2,3],omp_settings=dict(XSA_RESIDENT_DICT_THREADS=str(t),OMP_NUM_THREADS=str(t),OMP_THREAD_LIMIT=str(t),OMP_DYNAMIC='FALSE',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0'))
    base=dict(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22',XSA_GATE_PACKED_10G='1',XSA_RESIDENT_RADIX_LANES=str(lanes),XSA_RESIDENT_RADIX_STEP='16384')
    b['worker_controls']=dict(resident=dict(base,**b['omp_settings'],XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1'),sink=dict(base,XSA_TEST_RESIDENT_TRIPLES='1',XSA_BATCH_SXLS_ESCAPES=str(batch)),finish=base)
    if r:b['worker_controls']['sink']['XSA_PRIVATE_SXLS_RECLAIM']='1'
    markers=128 if r else 2*((TEN.n+1+262143)//262144);b['sxls_markers']=dict(reclaim=bool(r),bands_on_tmpfs=True,sampled_peak=markers,exact_final=markers)
    save(d/'build.json',b);(d/'build-source.log').write_text(PIN+'\n/tmp/src\n');(d/'cold.log').write_text('zero resident pages');(d/'seam-fixtures.log').write_text('PASS global seam LCP');(d/'exits.log').write_text('build=0 oracle=0')
    (d/'oracle.log').write_text(f'ORACLE rows={TEN.n+1} independent_final_rank={TEN.n+1} all_adjacent_sa_lcp_bwt_verified=1 above_u32={TEN.n-0xffffffff} fixture')
    workers=HISTORICAL_WORKERS if profile not in TEN_ARMS else dict(overlap_resident='a'*64,pfp_sort_r='b'*64)
    (d/'binaries.sha').write_text(HISTORICAL+'\n'+''.join(f'{h}  /tmp/{k}\n' for k,h in (FIXED|workers).items()))
    log=['RESIDENT_TRANSPORT row43=true text_width=8 lcp_width=8 rank_width=4','native_above_i32=true',f"RESIDENT_CHECK recurrence_ranks={f['m']} virtual_end=0",f"RESIDENT_STREAM rows={TEN.n+1} descriptors={f['descriptors']} groups=5 max_group=100",
         f"RESIDENT_PLAN parse_bound={bounds['parse']} aux_bound={bounds['aux']} dict_bound={bounds['sort']} emit_bound={bounds['emit']} array_grant={140<<30}"]+[f'RESIDENT_PHASE phase=x{i} seconds=1.25' for i in range(8)]
    if profile in TEN_ARMS:
        log.append(f'RESIDENT_DICT_SORT kernel=native_wide wide=true selected_threads={t} fs=0 thread_workspace_bytes={dict_extra(D,t)}');(d/'sink.log').write_text(f'SS_SXLS_WRITER batched_escapes={str(bool(batch)).lower()}')
    if r:
        with (d/'sink.log').open('a') as sink:sink.write('\nSS_PRIVATE_SXLS_RECLAIM window=64 resume=unsupported')
    if q:
        tiles=(D+16383)//16384
        log.extend([f'RESIDENT_RADIX_PLAN lanes={lanes} sa_ranks_per_tile=16384 block_bytes={65536*12} row_queue_reserved={q} raw_tiles={tiles} ranges={tiles} empty=0 adjusted=2 absorbed=0 heap_bound=400',f'RESIDENT_RADIX_JOIN lanes={lanes} ranges={tiles} blocks={tiles} lane_rows={[TEN.n//lanes]*lanes} serial_row_comparisons=0'])
    (d/'resident.log').write_text('\n'.join(log)+'\n')

class Matched(unittest.TestCase):
    def test_grid(self):
        self.assertEqual(len(TEN_ARMS),36)
        for profile in TEN_ARMS:self.assertEqual(config(profile),TEN)
        for bad in ('packed-10g','packed-10g-l1-b2-t4','packed-10g-l1-b1-t8','packed-radix-1-batch'):
            with self.assertRaises(ValueError):config(bad)
        self.assertEqual(dict_extra(2**31-1,2),2*(64+50176*4)+12291);self.assertEqual(dict_extra(2**31,4),4*(64+50176*8)+12291)
        self.assertEqual(emit_extra(16385,2),208+2*2*65536*12+(7<<20));self.assertEqual(emit_extra(16385,1),0)

    def test_markers(self):
        with TemporaryDirectory() as tmp:
            d=Path(tmp);(d/'producer.done').touch()
            for i in range(1,65):
                for suffix in ('done','consumed'):(d/f'band_{i}.lcp.{suffix}').touch()
            self.assertEqual(final_markers(d,65*262144,True),128)
            (d/'band_64.lcp.done').rename(d/'band_0.lcp.done')
            with self.assertRaises(AssertionError):final_markers(d,65*262144,True)

    def test_cpu_grant(self):
        with TemporaryDirectory() as tmp,patch('os.sched_getaffinity',return_value=set(range(8))):
            parent=Path(tmp);cg=parent/'unit';cg.mkdir();(cg/'cpu.max').write_text('400000 100000')
            with patch('overlap_resident_gate.CG',cg):
                self.assertEqual(omp_settings(4)['OMP_WAIT_POLICY'],'PASSIVE');(parent/'cpu.max').write_text('200000 100000')
                with self.assertRaises(RuntimeError):omp_settings(1)

    def test_device_counters(self):
        a={'io.stat':'8:1 rbytes=20 wbytes=5 rios=2'};b={'io.stat':'8:1 rbytes=30 wbytes=20\n8:2 rbytes=4 wbytes=6'}
        self.assertEqual(device_bytes(a,b),{'8:1':{'rbytes':10,'wbytes':15},'8:2':{'rbytes':4,'wbytes':6}})
        self.assertIsNone(device_bytes(None,b));self.assertIsNone(device_bytes(a,{'io.stat':None}))

    def test_deadline(self):
        with TemporaryDirectory() as tmp,patch('time.time',return_value=12):
            d=Path(tmp);self.assertFalse(expired(d,None));self.assertFalse(expired(d,13));self.assertTrue(expired(d,12));self.assertTrue((d/'stop.requested').exists())

    def test_pair(self):
        with TemporaryDirectory() as tmp:
            a,b,h=(Path(tmp)/p for p in ('a','b','old'));fixture(a,'packed-10g-l1-b1-t4-r1');fixture(b,'packed-10g-l4-b1-t4-r1');fixture(h,'packed-10g-bitmap')
            candidate=get(b,'build.json');candidate['raw_to_product_s']=300;save(b/'build.json',candidate)
            score=compare('single-knob',a,b);self.assertEqual(score['changed_knobs'],['lanes']);self.assertTrue(score['identical_verified_product']);self.assertEqual(score['trial_over_control_wall']['raw'],3)
            self.assertEqual(compare('historical',h,b)['changed_knobs'],['lanes','batch','dict_threads','reclaim'])
            with self.assertRaises(AssertionError):compare('single-knob',h,b)
            for fn,old,new in [('resident.log','selected_threads=4','selected_threads=1'),('resident.log','serial_row_comparisons=0','serial_row_comparisons=1'),('sink.log','true','false'),('resident.log','lane_rows=[2500000000','lane_rows=[2500000001')]:
                p=b/fn;orig=p.read_text();p.write_text(orig.replace(old,new));
                with self.assertRaises(AssertionError):verified_arm(b)
                p.write_text(orig)
            candidate['worker_controls']['finish']['XSA_PRIVATE_SXLS_RECLAIM']='1';save(b/'build.json',candidate)
            with self.assertRaises(AssertionError):verified_arm(b)
            del candidate['worker_controls']['finish']['XSA_PRIVATE_SXLS_RECLAIM'];save(b/'build.json',candidate)
            p=h/'binaries.sha';p.write_text(p.read_text().replace(HISTORICAL_WORKERS['pfp_sort_r'],'c'*64));
            with self.assertRaises(AssertionError):compare('historical',h,b)

if __name__=='__main__':unittest.main()
