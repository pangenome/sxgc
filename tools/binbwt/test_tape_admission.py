#!/usr/bin/env python3
"""Queued future bands must not time out while the consumer is making progress."""
import argparse
import os
from pathlib import Path
import random
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--scatter', required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='tape-admission-') as tmp:
    root = Path(tmp)
    data = random.Random(7).randbytes(8000000).translate(bytes(3+i%247 for i in range(256)))+b'\x1e'
    (root/'corpus').write_bytes(data)
    (root/'remap').write_bytes(bytes(range(256)))
    tape = root/'bands'; tape.mkdir()
    (root/'waves').mkdir()
    with (root/'order.log').open('w') as log:
        proc = subprocess.Popen(['timeout', '900', a.scatter, '--corpus', str(root/'corpus'),
            '--remap', str(root/'remap'), '--scratch', str(root/'waves'), '--tape-bands',
            str(tape), '--threads', '8', '--pending-bands', '1', '--budget-bytes', str(2<<30),
            '--no-lcp-slices'], env=dict(os.environ, XSA_TAPE_WAIT_S='.5'),
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT)
        band = rows = 0
        deadline = time.monotonic()+60
        try:
            while proc.poll() is None:
                assert time.monotonic() < deadline, 'test deadline'
                if (tape/f'band_{band}.done').exists():
                    # Eight queued workers used to accumulate >0.5 s of wait,
                    # although each actual handshake takes only 0.1 s.
                    time.sleep(.1)
                    path = tape/f'band_{band}.sa'
                    rows += len(path.read_bytes())//8
                    path.unlink()
                    (tape/f'band_{band}.consumed').touch()
                    band += 1
                else:
                    time.sleep(.001)
            assert proc.returncode == 0, (root/'order.log').read_text()
            assert rows == len(data)+1, (rows,len(data)+1)
            print(f'PASS ordered admission bands={band} rows={rows} deadline=.5 handshake=.1')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=10)
