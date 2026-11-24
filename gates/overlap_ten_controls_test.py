"""Read-only score regression + tiny allowlisted, stable /proc child capture. No corpus access."""
import copy
from pathlib import Path
import os
import subprocess as sp
import sys
import tempfile
import unittest
from unittest.mock import patch
import overlap_ten_controls as c
import overlap_ten_resume as r
from overlap_ten_score import measured_goals

class Controls(unittest.TestCase):
    def test_audited_original_and_misses(self):
        r.confirm_pin() if os.environ.get('SXGC_TEST_LOCAL_PINS')=='1' else None
        d=r.STUDY/c.FIRST;b=r.load(d/'build.json');expected=r.wanted(c.FIRST)
        self.assertTrue(c.evidence(d,b,expected).startswith('exact first-arm'));self.assertTrue(measured_goals(d)==r.load(d/'goals.json'))
        bad=copy.deepcopy(b);bad['worker_controls']['sink']={'XSA_PRIVATE_SXLS_RECLAIM':'0'}
        with self.assertRaises(AssertionError):c.evidence(d,bad,expected)
        bad=copy.deepcopy(b);bad['profile']=r.FIRST.replace('l4','l1')
        with self.assertRaises(AssertionError):c.evidence(d,bad,expected)
        text=r.load(d/c.MISSING)['traceback'];self.assertTrue(r.original_postscore(1,'PASS resident ranks, all raw SA/LCP/BWT rows, exact nine-member SHA x\n'+text,d));self.assertFalse(r.original_postscore(1,text,d));self.assertFalse(r.original_postscore(75,text,d))
    def test_stable_sample(self):
        with tempfile.TemporaryDirectory() as tmp:
            d=Path(tmp);py=Path(sys.executable).resolve();env={'PFCK_SPACING':'22','UNRELATED_PRIVATE':'do not record'}
            with sp.Popen([py,'-c','import sys;print(1,flush=True);sys.stdin.read(1)',d/'corpus'],stdin=sp.PIPE,stdout=sp.PIPE,env=env) as p:
                try:
                    self.assertEqual(p.stdout.readline(),b'1\n');v={'PFCK_SPACING':'22'}
                    self.assertEqual(c.stable_read(p.pid,v),v);cg,s=r.sample(os.getpid(),d,'test',py,v)
                    self.assertEqual(s['controls'],v);self.assertTrue(s['same_executable_inode']);self.assertEqual(s['pid'],p.pid)
                    with self.assertRaises(RuntimeError):r.sample(os.getpid(),d,'test',py,{'PFCK_SPACING':'11'})
                finally:p.stdin.write(b'x');p.stdin.flush()
    def test_bad_evidence_or_unrelated_variables(self):
        self.assertEqual(c.environ(b'TOKEN=secret\x00XSA_RESIDENT_DICT_THREADS=4\x00'),{'XSA_RESIDENT_DICT_THREADS':'4'})
        with self.assertRaises(RuntimeError):c.environ(b'PFCK_SPACING=22\x00PFCK_SPACING=22\x00')
if __name__=='__main__':unittest.main()
