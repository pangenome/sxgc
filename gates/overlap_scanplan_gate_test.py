"""Exercise actual gate orchestration against tiny bytes and fake kernel counters."""
import hashlib
import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

import overlap_scanplan_gate as gate


class ScanGate(unittest.TestCase):
    def test_guard(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(gate,'current_io',return_value=None), \
             patch.object(gate,'worker_io',side_effect=[{'read_bytes':1,'write_bytes':0},
                                                        {'read_bytes':4,'write_bytes':0}]):
            d = Path(tmp); report = {}
            phase = gate.Phase(d,report,'test',time.time()+60,2,0)
            with self.assertRaisesRegex(RuntimeError,'IO guard'):
                phase.guard(True)
            self.assertEqual(report['test']['worker_io']['read_bytes'],3)
            self.assertIsNone(report['test']['per_device_io'])
            (d/'stop.requested').touch()
            with self.assertRaisesRegex(RuntimeError,'deadline/stop'):
                phase.guard()

    def test_missing_control_refused_before_source(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(gate,'exclusive'), \
             patch.object(gate,'cg_info',return_value={'cpu.max':'100000 100000',
                 'memory.max':str(192<<20),'memory.swap.max':'0'}), \
             patch.object(gate,'CG',Path(tmp)), patch.object(gate,'RECEIPTS',Path(tmp)):
            (Path(tmp)/'cgroup.procs').write_text(str(gate.os.getpid()))
            with self.assertRaises(FileNotFoundError):
                gate.calibrate(Path(tmp),time.time()+60,{})
            self.assertEqual(list(Path(tmp).iterdir()),[Path(tmp)/'cgroup.procs'])

    def test_published_control(self):
        self.assertEqual(gate.metrics(gate.RECEIPTS/'packed-serial-10g')['profile'],'packed-10g-bitmap')
        front = (gate.RECEIPTS/'packed-front-10g/front-end.json').read_bytes()
        self.assertEqual(hashlib.sha256(front).hexdigest(),'5e875c44f9293e35471e2b30645f15cb02c8a44164aa4adcd679144e688c06bf')
        self.assertEqual(json.loads(front)['census']['n'],gate.TEN)


if __name__ == '__main__':
    unittest.main()
