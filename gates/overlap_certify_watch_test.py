"""Synthetic file-close observations; never start a construction job."""
import hashlib
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest
from overlap_certify_watch import observe

class Watch(unittest.TestCase):
    def test_three(self):
        with tempfile.TemporaryDirectory() as root:
            root=Path(root);s=root/'sandbox';s.mkdir();dest=root/'dest';dest.mkdir();prof=[f'packed-10g-l{i}-b1-t4-r1' for i in (4,2,1)]
            dirs=[s/(p+'-ABCdef') for p in prof];dirs[0].mkdir();e=threading.Event()
            def writer():
                if not e.wait(2):return
                for d in dirs:
                    if d!=dirs[0]:d.mkdir();time.sleep(.08)
                    (d/'noise').touch();(d/'exits.log').write_bytes(b'build=0 oracle=0\n');time.sleep(.09);(d/'identity.json').write_bytes(b'{}')
            t=threading.Thread(target=writer);t.start()
            try:observe(s,dirs[0],prof,dest,time.time()+5,e.set)
            finally:t.join(timeout=2)
            for d,p in zip(dirs,prof):
                j=json.loads((dest/(p+'.json')).read_text());self.assertEqual(j['source'],str(d));self.assertEqual(j['close_events']['identity.json'][2],hashlib.sha256(b'{}').hexdigest());self.assertGreater(j['filesystem_mtime_delta_s'],.04)
            z=root/'late';z.mkdir()
            with self.assertRaisesRegex(ValueError,'late observer'):observe(s,dirs[0],prof,z,time.time()+2)
if __name__=='__main__':unittest.main()
