"""Admission and private-file orchestration tests: no dataset or service."""
from contextlib import ExitStack
from copy import deepcopy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import overlap_scanplan_hundred as hundred


def cg():
    return {'memory.max':str(192<<20),'memory.peak':'1024','memory.events':'oom 0\noom_kill 0',
            'cpu.max':'100000 100000','memory.swap.max':'0'}


def example():
    n = hundred.TEN; margin = hundred.gate.MARGIN; h = '1'*64
    c = dict(n=n,m=100,alphabet=1,max_phrase=100000008,starts_above_u32=50,eof_start=n-100000000,
             eof_raw_length=100000000,max_owned_stride=100000000,global_end_rows=1,cells=[[0,1],[1,n-1]],sha256=h)
    bank = {k:c[k] for k in hundred.FIELDS}; page=os.sysconf('SC_PAGE_SIZE')
    s = dict(status='CALIBRATED_10G_SCAN_ONLY',brute_fixture='passed',launch_authorized=False,
             source=str(hundred.gate.RUNGS['10g'][1]),source_stat=[1,2,n,0,0],kernel_sha256=h,copy_sha256=h,
             private_copy_budget=n,private_copy_bytes=n,counted_column_scratch_bytes=0,mincore_resident_pages=0,
             mincore_checked_pages=(n+page-1)//page,free_before=n+(1<<30),free_after=margin,banked_fields=bank.copy(),
             census=c, models=[hundred.plan(c,x) for x in (256<<20,1<<30)],cgroup_before=cg(),cgroup_after=cg())
    for name in ('copy_cold','raw_scan'):
        s[name]=dict(read_cap=n+margin,write_cap=n+margin if name=='copy_cold' else margin,
                     wall_s=10.,worker_cpu_s=8.,per_device_io=None,
                     worker_io=dict(read_bytes=n,write_bytes=n if name=='copy_cold' else 0))
    return s,bank


class Hundred(unittest.TestCase):
    def test_calibration_replay(self):
        s,bank=example(); self.assertTrue(all(model['ranges'] is None for model in s['models']))
        self.assertEqual(hundred.calibration_clock(s,bank),20)
        for path,bad in [('status','fixture'),('private_copy_bytes',1000),('mincore_resident_pages',1),
                         ('source','/tmp/tiny'),('copy_sha256','2'*64),('banked_fields.m',4),
                         ('census.cells',[[0,1],[1,1]]),('models',[]),('raw_scan.worker_io.read_bytes',0),
                         ('copy_cold.worker_io.write_bytes',0),('raw_scan.read_cap',10**15),
                         ('copy_cold.per_device_io',{'8:1':{'rbytes':-1,'wbytes':0}}),
                         ('cgroup_after.memory.events','oom 1\noom_kill 0'),('cgroup_before.memory.swap.max','max'),
                         ('cgroup_after.memory.peak',str(193<<20)),('raw_scan.wall_s',float('nan'))]:
            v=deepcopy(s); p=v; parts=path.split('.')
            while '.'.join(parts) not in p: p=p[parts.pop(0)]
            p['.'.join(parts)]=bad
            with self.subTest(path=path), self.assertRaises(RuntimeError): hundred.calibration_clock(v,bank)

    def test_documented_replayed_default(self):
        v=dict(rung='10g',profile='packed-10g-l2-b0-t4-r1',normalized_speed_target_met=False)
        data=json.dumps(v).encode(); docs=v['profile']+' '+hundred.sha(data)
        hundred.verify_default(data,docs,v)
        for d,doc in [(data,bad) for bad in ('',v['profile'],hundred.sha(data))]+[(b'{}',docs)]:
            with self.assertRaises(RuntimeError): hundred.verify_default(d,doc,v)
        one=dict(v,rung='1g'); data=json.dumps(one).encode()
        with self.assertRaises(RuntimeError): hundred.verify_default(data,docs+' '+hundred.sha(data),one)

    def test_complete_sweep_provenance(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp); selected='packed-10g-l2-b0-t4-r1'
            for lane in (1,2,4):
                arm=d/selected.replace('l2',f'l{lane}'); arm.mkdir()
                with self.assertRaises(FileNotFoundError): hundred.check_sweep(d,selected)
                (arm/'build.json').write_text(json.dumps(dict(profile=arm.name)))
                (arm/'source.json').write_text(json.dumps(dict(complete_arm=True,identity=True)))
            hundred.check_sweep(d,selected)
            (arm/'stop.requested').touch()
            with self.assertRaises(RuntimeError): hundred.check_sweep(d,selected)
            (arm/'stop.requested').unlink(); (arm/'source.json').write_text('{"complete_arm":false,"identity":true}')
            with self.assertRaises(RuntimeError): hundred.check_sweep(d,selected)
            with self.assertRaises(RuntimeError): hundred.check_sweep(d,'packed-radix-4-batch')

    def test_cutoff_and_copy_inclusive_clock(self):
        end=hundred.CUTOFF
        with patch.object(hundred.time,'time',return_value=end-480):
            self.assertEqual(hundred.budget(end,20),480)
            for d in (end-1,end+1):
                with self.assertRaises(RuntimeError): hundred.budget(d,20)

    def test_missing_or_changed_before_metadata(self):
        for missing in (True,False):
            with tempfile.TemporaryDirectory() as tmp, ExitStack() as st:
                d=Path(tmp); (d/'cgroup.procs').write_text(str(os.getpid()))
                st.enter_context(patch.object(hundred.gate,'CG',d)); st.enter_context(patch.object(hundred.gate,'cg_info',return_value=cg()))
                st.enter_context(patch.object(hundred,'admitted',side_effect=FileNotFoundError() if missing else None,return_value={'commit':'moved'}))
                inventory=st.enter_context(patch.object(hundred.contract,'inventory'))
                with self.assertRaises(FileNotFoundError if missing else RuntimeError): hundred.scan(d,hundred.CUTOFF,'0'*64,{})
                inventory.assert_not_called()

    def test_anonymous_copy_only_after_pin_and_publication(self):
        data=b'byte fixture only'; digest=hundred.sha(data)
        for outcome in ('moved','wrong_sha','ok'):
            with self.subTest(outcome=outcome),tempfile.TemporaryDirectory() as tmp,ExitStack() as st:
                mock=lambda obj,name,**kw: st.enter_context(patch.object(obj,name,**kw))
                d=Path(tmp); source=d/'pile-head100.txt'; source.write_bytes(data)
                (d/'cgroup.procs').write_text(str(os.getpid()))
                st.enter_context(patch.multiple(hundred.contract,ROOT=d,N=len(data),CORPUS=digest if outcome!='wrong_sha' else '0'*64))
                st.enter_context(patch.object(hundred.gate,'CG',d)); mock(hundred.gate,'cg_info',return_value=cg())
                ticket=dict(commit='admitted',calibration_kernel='kernel',calibration_clock_s=1)
                mock(hundred,'admitted',return_value=ticket); pin=hundred.sha(json.dumps(ticket,sort_keys=True).encode())
                mock(hundred.defaults,'published_main',return_value='other' if outcome=='moved' else 'admitted')
                mock(hundred.gate,'exclusive'); mock(hundred,'budget'); mock(hundred.time,'time',return_value=100)
                inventory=mock(hundred.contract,'inventory',return_value=dict(corpus_path=str(source),corpus_stat_bytes=len(data),manifest_only_sha256=hundred.contract.CORPUS))
                mock(hundred.gate,'build_kernel',side_effect=lambda d,t,r,g:r.update(kernel_sha256='kernel'))
                def inspect_private(path):
                    self.assertEqual(path.stat().st_nlink,0); self.assertEqual(path.stat().st_mode&0o777,0o700)
                    self.assertFalse(path.samefile(source)); self.assertEqual(path.read_bytes(),data)
                cold=mock(hundred.gate,'cold',side_effect=inspect_private)
                mock(hundred.gate,'current_io',return_value=None)
                counter=iter(range(100)); mock(hundred.gate,'worker_io',side_effect=lambda:dict(read_bytes=next(counter),write_bytes=0))
                def tiny(lib,raw,n,guard):
                    guard(); self.assertEqual(raw.read(),data)
                    return dict(sha256=digest,n=n,m=1,cells=[[0,n]],max_phrase=n,eof_raw_length=n)
                scanner=mock(hundred,'census',side_effect=tiny); r={}
                if outcome=='ok':
                    hundred.scan(d,hundred.CUTOFF,pin,r); self.assertEqual(r['status'],'MEASURED_100G_SCAN_ONLY')
                    scanner.assert_called_once(); cold.assert_called_once()
                else:
                    with self.assertRaises(RuntimeError): hundred.scan(d,hundred.CUTOFF,pin,r)
                    scanner.assert_not_called(); cold.assert_not_called()
                    if outcome=='moved': inventory.assert_not_called()


if __name__=='__main__': unittest.main()
