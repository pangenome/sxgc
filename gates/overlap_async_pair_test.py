"""Synthetic score/refusal fixtures based on the small archived 1 GB receipt. No corpus, service, or tee run."""
import json
import math
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest.mock import patch
import overlap_async_pair as a


def synthetic(d,arm):
    # Temporary fake *instrumentation* to exercise both branches of the validator.
    # Never write a study bank or treat either shape as a measured async product.
    shutil.copytree(a.REF,d);b=a.load(d,'build.json');f=a.load(d,'front-end.json');rev=(d/'binaries.sha').read_text().splitlines()[0]
    base=dict(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22',XSA_RESIDENT_RADIX_LANES='4',XSA_RESIDENT_RADIX_STEP='16384')
    res=dict(base,XSA_RESIDENT_PACKED='1',XSA_RESIDENT_WIDE_DICT='1');t=arm=='async';extra=a.ASYNC_RESERVE if t else 0
    if t:res['XSA_RESIDENT_ASYNC_AUDIT']='1'
    b.update(audit_pair_arm=arm,worker_controls=dict(resident=res,sink=dict(base,XSA_TEST_RESIDENT_TRIPLES='1',XSA_BATCH_SXLS_ESCAPES='1'),finish=base),tee_reserve_array_bytes=extra,verifier_transport='dedicated inherited FIFO',cpu_affinity=[0,1,2,3],prepared_cgroup_before=b['shared_before'],sxls_markers=dict(reclaim=False,exact_final=2*((a.N+262144)//262144)))
    logfile=d/'resident.log';s=logfile.read_text();orig=f['array_bounds']['emit'];f['array_bounds']['emit']+=extra;s=s.replace(f'emit_bound={orig} ',f"emit_bound={f['array_bounds']['emit']} ")
    if t:s+='RESIDENT_ASYNC_AUDIT data_bytes=%d sink_bytes=%d audit_bytes=%d peak_inflight_blocks=8 enqueue_s=1 worker_write_s=1 worker_wall_s=1 drain_s=0\n'%(12*a.N,12*(a.N+1),12*(a.N+1))
    logfile.write_text(s);a.save(d/'front-end.json',f);a.save(d/'build.json',b)
    p={k:[1,200+int(t)*10+i,n,3,4] for i,(k,(_,n)) in enumerate(a.FILES.items())};o={k:[9,10+i,n,3,4] for i,(k,(_,n)) in enumerate(a.FILES.items())};o['oracle-corpus']=o['in/corpus']
    cg={'memory.max':str(192<<20),'memory.peak':str(100<<20),'cpu.max':'100000 100000','memory.swap.max':'0','memory.events':'oom 0\noom_kill 0'};page=os.sysconf('SC_PAGE_SIZE');cap=3*a.N+a.MARGIN
    a.save(d/'prep.json',dict(status='PRIVATE_COLD',arm=arm,revision=rev,private_stat=p,source_stat=o,sha256={'in/remap':a.binaries(a.REF)['remap'],'in/corpus':a.ONE_RAW_SHA,'oracle-corpus':a.ONE_RAW_SHA,'inverse.check':'5'*64},mincore_resident_pages=0,mincore_checked_pages={k:math.ceil(v[1]/page) for k,v in a.FILES.items()},cgroup_before=cg,cgroup_after=cg,private_copy_and_cold=dict(read_cap=cap,write_cap=cap,wall_s=10,worker_cpu_s=3,worker_io=dict(read_bytes=0,write_bytes=sum(n for _,n in a.FILES.values())),per_device_io=None)))
    (d/'certify.log').write_text('cert_process_wall_s=0.72 cpu_user_s=0.33 cpu_system_s=0.31\n')

class Pair(unittest.TestCase):
    def test_bounded_deadline(self):
        with patch.object(a.time,'time',return_value=a.CUTOFF-1600):
            self.assertEqual(a.ensure_time(a.CUTOFF,305,2),1367)
            for end,arms in [(a.CUTOFF-1000,2),(a.CUTOFF+1,2),(a.CUTOFF,3)]:
                with self.subTest(end=end,arms=arms),self.assertRaises(RuntimeError):a.ensure_time(end,305,arms)
    def test_pair_single_knob(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);s=d/'sync';b=d/'async';synthetic(s,'sync');synthetic(b,'async');z=a.score(d)
            self.assertTrue(z['verified_full_products']);self.assertFalse(z['matched_product_speed_io_cpu_pass']);self.assertEqual(z['time_ratio']['raw'],1);self.assertIsNone(z['sync']['raw_device_io'])
            for name,old,new in [('resident.log','peak_inflight_blocks=8','peak_inflight_blocks=9'),('resident.log','data_bytes='+str(12*a.N),'data_bytes=0'),('oracle.log','all_adjacent_sa_lcp_bwt_verified=1','hash_sink_only=1'),('certify.log','wall_s=0.72','wall_s=0.00'),('build.json','"XSA_RESIDENT_ASYNC_AUDIT": "1"','"XSA_RESIDENT_ASYNC_AUDIT": "0"'),('prep.json','"mincore_resident_pages": 0','"mincore_resident_pages": 1'),('prep.json',a.ONE_RAW_SHA,'0'*64),('exits.log','build=0 oracle=0','build=0 oracle=1')]:
                p=b/name;v=p.read_text();self.assertIn(old,v);p.write_text(v.replace(old,new))
                with self.subTest(name=name,old=old),self.assertRaises((AssertionError,RuntimeError)):a.score(d)
                p.write_text(v)
            for field,bad in [('sha256',{**a.load(b,'prep.json')['sha256'],'inverse.check':'6'*64}),('private_stat',a.load(s,'prep.json')['private_stat'])]:
                p=b/'prep.json';v=p.read_bytes();a.save(p,dict(json.loads(v),**{field:bad}))
                with self.subTest(field=field),self.assertRaises(RuntimeError):a.score(d)
                p.write_bytes(v)
            v=a.load(b,'build.json');v['raw_to_product_s']-=.06*v['wall_s'];v['wall_s']*=.94;a.save(b/'build.json',v);self.assertTrue(a.score(d)['matched_product_speed_io_cpu_pass'])
            v['raw_worker_io']['write_bytes']+=v['process']['finish']['write_bytes'];v['process']['finish']['write_bytes']*=2;a.save(b/'build.json',v);self.assertFalse(a.score(d)['matched_product_speed_io_cpu_pass'])

if __name__=='__main__':unittest.main()
