#!/usr/bin/env python3
"""Returning band 1's credit must admit band 2 while band 0 remains unread."""
import argparse
import os
from pathlib import Path
import random
import subprocess
import tempfile
import time

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--scatter',required=True)
a=p.parse_args()
with tempfile.TemporaryDirectory(prefix='tape-any-credit-') as tmp:
    root=Path(tmp);tape=root/'bands';tape.mkdir();(root/'waves').mkdir()
    (root/'corpus').write_bytes(random.Random(7).randbytes(8000000).translate(bytes(3+i%247 for i in range(256))))
    (root/'remap').write_bytes(bytes(range(256)))
    with (root/'order.log').open('w') as log:
        proc=subprocess.Popen([a.scatter,'--corpus',str(root/'corpus'),'--remap',str(root/'remap'),
            '--scratch',str(root/'waves'),'--tape-bands',str(tape),'--pending-bands','2',
            '--threads','96','--budget-bytes',str(1<<30),'--no-lcp-slices'],
            stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,XSA_TAPE_WAIT_S='180'))
        def wait_for(path):
            deadline=time.monotonic()+30
            while not path.exists():
                assert proc.poll() is None,(root/'order.log').read_text()
                assert time.monotonic()<deadline,(root/'order.log').read_text()
                time.sleep(.01)
        try:
            wait_for(tape/'band_1.done')
            payload=(tape/'band_1.sa').read_bytes()
            (tape/'band_1.sa').unlink();(tape/'band_1.consumed').touch()
            wait_for(tape/'band_2.done')
            assert not (tape/'band_0.consumed').exists()
            print('PASS band 1 credit admitted band 2 while band 0 remained unread')
        finally:
            if proc.poll() is None:
                proc.terminate();proc.wait(timeout=10)
