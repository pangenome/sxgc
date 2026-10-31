#!/usr/bin/env python3
"""Disk credit must precede downstream publication, even for multi-chunk bands."""
import argparse
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--merge', required=True)
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='tape-release-') as tmp:
    root = Path(tmp)
    tape = root/'bands'
    tape.mkdir()
    rng = random.Random(123)
    data = bytes(rng.randrange(3, 250) for _ in range(200000))
    (root/'corpus').write_bytes(data)
    (root/'remap').write_bytes(bytes(range(256)))
    n = len(data)+1
    (tape/'manifest').write_text(f'{n} 1\n')
    (tape/'ready').touch()
    (tape/'end').write_text('3\n')
    (tape/'end.ready').touch()
    # A permutation suffices: this test checks transport ownership, not sorting.
    packed = b''.join(struct.pack('<Q', i | (0 if i == 0 else data[i-1]) << 48)
                      for i in range(n))
    for band, payload in enumerate((b'', packed, b'')):
        (tape/f'band_{band}.sa').write_bytes(payload)
        (tape/f'band_{band}.done').touch()
    env = {k:v for k,v in os.environ.items() if not k.startswith(('CROSS_', 'XSA_'))}
    env.update(CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1')
    with (root/'emit.log').open('w') as log:
        proc = subprocess.Popen(['timeout', '900', a.merge, '--binbwt', str(root/'corpus'),
            '--remap', str(root/'remap'), '--out-prefix', str(root/'frag'), '--threads', '3',
            '--tape-bands', str(tape), '--tape-emit-chunk', '1024'],
            stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            deadline = time.monotonic()+15
            while not (tape/'band_1.consumed').exists():
                assert proc.poll() is None, (root/'emit.log').read_text()
                assert time.monotonic() < deadline, 'band credit retained while sink waits for absent finish'
                time.sleep(.01)
            assert (tape/'band_0.consumed').exists()
            assert not (tape/'band_0.sa').exists()
            assert not (tape/'band_1.sa').exists()
            assert proc.poll() is None, 'expected downstream backpressure'
            (root/'frag.runcols.consumed').write_text('100000\n')
            assert proc.wait(timeout=15) == 0, (root/'emit.log').read_text()
            assert (tape/'band_2.consumed').exists()
            assert not (tape/'band_2.sa').exists()
            print('PASS multi-chunk release before publication; leading/trailing empty bands acknowledged')
        finally:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
