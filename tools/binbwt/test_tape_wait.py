#!/usr/bin/env python3
"""A diagnostic must report a stalled consumer without terminating the sorter."""
import argparse
import os
import random
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--scatter', required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='tape-wait-') as tmp:
    root = Path(tmp)
    (root/'corpus').write_bytes(random.Random(7).randbytes(2000000).translate(bytes(3+i%247 for i in range(256)))+b'\x1e')
    (root/'remap').write_bytes(bytes(range(256)))
    (root/'bands').mkdir()
    (root/'waves').mkdir()
    with (root/'order.log').open('w') as log:
        run = subprocess.Popen([a.scatter, '--corpus', str(root/'corpus'),
        '--remap', str(root/'remap'), '--scratch', str(root/'waves'), '--tape-bands',
        str(root/'bands'), '--threads', '2', '--pending-bands', '1',
        '--budget-bytes', str(2<<30), '--no-lcp-slices'],
        env=dict(os.environ, XSA_TAPE_WAIT_S='.2'), stdin=subprocess.DEVNULL,
        stdout=log, stderr=subprocess.STDOUT)
        try:
            import time
            deadline = time.monotonic()+15
            while 'SS_DIAGNOSTIC ' not in (root/'order.log').read_text():
                assert run.poll() is None, (root/'order.log').read_text()
                assert time.monotonic() < deadline, 'missing diagnostic'
                time.sleep(.02)
            time.sleep(.3)
            assert run.poll() is None, 'diagnostic terminated producer'
            record = next(line for line in (root/'order.log').read_text().splitlines()
                          if line.startswith('SS_DIAGNOSTIC '))
            fields = (['queue=band', 'capacity=1', 'reservations=1', 'wall_s=']
                      if 'queue=band' in record else
                      ['writer=emit-tape(cross_lcp_merge)', 'band_exists=1', 'merge_state={unavailable}'])
            for field in fields:
                assert field in record, record
            print(record)
            print('PASS diagnostic does not terminate stalled producer')
        finally:
            if run.poll() is None:
                run.terminate(); run.wait(timeout=10)
