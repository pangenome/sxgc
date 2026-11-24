"""Budget, readout and cooperative meter tests; no corpus or actual service."""
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import overlap_vocab_gate as g

class VocabGate(unittest.TestCase):
    def test_10g_bounds_and_checks(self):
        c,f=g.banks();v=g.prospective(c['n'],c['m'],c['max_phrase'])
        self.assertEqual(v['first_pass_bytes'],13_817_973_848);self.assertLess(v['counted_bound'],2*c['n']);self.assertLess(v['logical_scratch_traffic_upper'],24*c['n'])
        self.assertEqual(f['k'],234472723);self.assertEqual(f['descriptors'],9129290112)
        for args in [(1000000,999999,8),(10**10,10,1<<21)]:
            with self.subTest(args=args),self.assertRaises(RuntimeError):g.prospective(*args)
        with patch.object(g,'exclusive') as ex,self.assertRaises(RuntimeError):g.admitted(0)
        ex.assert_not_called()
    def test_sparse(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp)/'out';p.write_text('n=4 m=1\n0 1 1\n1 2 3\n');v,d,o=g.readout(p);self.assertEqual(v['n'],sum(o));self.assertEqual(sum(d),3)
            for extra in ('1 2 4\n','2 2\n','65792 1 1\n'):
                p.write_text('n=4\n1 1 1\n'+extra)
                with self.assertRaises(ValueError):g.readout(p)
    def test_meter_success_and_cooperative_flag(self):
        fake={'io.stat':None,'memory.events':'oom 0\noom_kill 0'}
        with tempfile.TemporaryDirectory() as tmp,patch.object(g,'grant',return_value=fake),patch.object(g,'cg_info',return_value=fake),patch.object(g,'current_io',return_value=None):
            d=Path(tmp);rec={};cmd=[sys.executable,'-c','import time;time.sleep(.15);print(42)'];g.meter(d,cmd,'census',time.time()+5,rec)
            self.assertEqual(rec['census']['rc'],0);self.assertIsNone(rec['census']['device_io']);self.assertIn('read_bytes',rec['census']['process'])
            sub=d/'stopped';sub.mkdir();cmd=[sys.executable,'-c',"from pathlib import Path\nimport time\nwhile not Path('stop.requested').exists():time.sleep(.02)\nraise SystemExit(75)"]
            with self.assertRaises(RuntimeError):g.meter(sub,cmd,'census',time.time()+.1,rec)
            self.assertEqual(rec['census']['rc'],75);self.assertTrue((sub/'stop.requested').exists())
if __name__=='__main__':unittest.main()
