"""In-memory structural receipt and arithmetic tests; no 100 GB input, inventory, git publication or dispatch."""
import copy
import json
import math
import os
import unittest
from contextlib import ExitStack
from unittest.mock import patch
import overlap_hundred_result as r

class Result(unittest.TestCase):
    def test_bounds_and_missing_observations(self):
        n=10_000_000_000
        c={'n':n,'m':318164488,'max_phrase':88468,'max_owned_stride':88460,'eof_start':n-80,'eof_raw_length':80,'global_end_rows':1,'starts_above_u32':181581998,'alphabet':1,'cells':[[0,1],[1,n-1]]}
        v=r.dimensions(c);self.assertEqual(v['U_upper'],n);self.assertFalse(v['narrow_parse_impossible_from_m_alone']);self.assertEqual(v['largest_k_for_i32_extent'],(r.I32-c['m'])//6)
        for k,x in [('eof_start',n),('starts_above_u32',0),('m',0),('cells',[[0,n]]),('max_phrase',10),('alphabet',0)]:
            with self.subTest(k=k),self.assertRaises((ValueError,RuntimeError)):r.dimensions({**c,k:x})
    def test_complete_offline_shape_and_corruption(self):
        h=r.h;n=r.N;rev='1'*40;H='a'*64;cg=dict([('memory.max',str(192<<20)),('cpu.max','100000 100000'),('memory.swap.max','0'),('memory.peak',str(192<<20)),('memory.events','oom 0\noom_kill 0')]);data=lambda o:json.dumps(o).encode()
        files={k:b'fixture' for k in h.DOCS};db={}
        for rung,f in [('1g',h.ONE_DEFAULT),('10g',h.DEFAULT)]:
            v=db[rung]=dict(rung=rung,profile='fixture-'+rung,normalized_speed_target_met=None if rung=='1g' else True,raw_s=3500.);b=files[f]=data(v);files[h.DOCS[0]]+=b' '+v['profile'].encode()+b' '+h.sha(b).encode()
        for f in (h.FRONT,h.SCAN):files[f]=(h.ROOT/f).read_bytes()
        cal=json.loads(files[h.SCAN]);t=h.calibration_clock(cal,json.loads(files[h.FRONT])['census']);c=dict(n=n,sha256=H,m=3_000_000_000,max_phrase=40000,max_owned_stride=39992,eof_start=n-20,eof_raw_length=20,starts_above_u32=2_800_000_000,alphabet=1,cells=[[0,1],[1,n-1]],global_end_rows=1)
        a=dict(commit=rev,receipt_tree='tree',profile='fixture-10g',under_2x=True,hashes={k:h.sha(v) for k,v in files.items()},calibration_clock_s=t,required_s=math.ceil(15*t+180),calibration_kernel=cal['kernel_sha256']);end=h.CUTOFF
        s=dict(status='MEASURED_100G_SCAN_ONLY',admission=a,launch_authorized=False,product_promotion=False,metadata_match=True,immutable_read_fd=True,brute_fixture='passed',kernel_sha256=cal['kernel_sha256'],source=str(h.contract.ROOT/'pile-head100.txt'),copy_sha256=H,manifest_sha256=H,source_stat=[1,2,n,3,4],private_copy_budget=n,private_copy_bytes=n,census=c,free_before=3*n,free_after_scan=2*n,mincore_resident_pages=0,mincore_checked_pages=math.ceil(n/os.sysconf('SC_PAGE_SIZE')),private_copy_allocated=n,whole_run_allocated_before_receipt=n+50000,conservative_scan_reservation=h.reservation(n),counted_limit=2*n,counted_column_scratch_bytes=0,cgroup_before=cg,cgroup_after=cg,models=[h.plan(c,z) for z in (256<<20,1<<30)],deadline=end,closed_unix_ns=(end-1000)*10**9,opened_unix_ns=(end-2000)*10**9)
        for name in ('copy_cold','raw_scan'):s[name]=dict(read_cap=n+h.gate.MARGIN,write_cap=h.gate.MARGIN+(n if name=='copy_cold' else 0),wall_s=100.,worker_cpu_s=90.,per_device_io=None,worker_io=dict(read_bytes=n,write_bytes=n if name=='copy_cold' else 0))
        with ExitStack() as st:
            st.enter_context(patch.object(h.contract,'CORPUS',H));st.enter_context(patch.object(h,'git',return_value='tree'));st.enter_context(patch.object(r,'blob',side_effect=lambda rev,p:files[p]));st.enter_context(patch.object(h.defaults,'decision',side_effect=lambda rung:db[rung]))
            out=r.verify(s);self.assertTrue(out['dimensions']['narrow_parse_impossible_from_m_alone']);self.assertEqual(out['speed_factor_needed_vs_linear_just_to_fit_clock'],35.)
            for key,bad in [('scan',''),('models',[]),('private_copy_allocated',4*n),('source','/tmp/wrong'),('closed_unix_ns',end*10**9+1),('immutable_read_fd',False),('raw_scan',dict(s['raw_scan'],per_device_io={'8:0':dict(rbytes=-1,wbytes=0)})),('admission',dict(a,receipt_tree='bad'))]:
                v=copy.deepcopy(s)
                if key=='scan':v['census']['sha256']='different'
                else:v[key]=bad
                with self.subTest(key=key),self.assertRaises(RuntimeError):r.verify(v)
if __name__=='__main__':unittest.main()
