#!/usr/bin/env python3
"""A missing tape consumer must fail quickly and name its ownership state."""
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
    run = subprocess.run(['timeout', '900', a.scatter, '--corpus', str(root/'corpus'),
        '--remap', str(root/'remap'), '--scratch', str(root/'waves'), '--tape-bands',
        str(root/'bands'), '--threads', '2', '--pending-bands', '1',
        '--budget-bytes', str(2<<30), '--no-lcp-slices'],
        env=dict(os.environ, XSA_TAPE_WAIT_S='.2'), stdin=subprocess.DEVNULL,
        capture_output=True, text=True, timeout=30)
    assert run.returncode == 2, run.stderr
    assert "SS_TIMEOUT " in run.stderr, run.stderr
    record = next(line for line in run.stderr.splitlines() if line.startswith('SS_TIMEOUT '))
    for field in ['writer=emit-tape(cross_lcp_merge)', 'band_exists=1', 'band_bytes=',
                  'merge_opened=0', 'merge_state={unavailable}']:
        assert field in record, record
    print(record)
    print('PASS missing consumer fails with XSA_TAPE_WAIT_S=.2')
