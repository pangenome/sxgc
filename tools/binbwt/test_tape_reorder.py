#!/usr/bin/env python3
"""A missing low band must not prevent read/release of a completed high band."""
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
with tempfile.TemporaryDirectory(prefix='tape-reorder-') as tmp:
    root = Path(tmp)
    tape = root/'bands'; tape.mkdir()
    data = random.Random(123).randbytes(200000).translate(bytes(3+i%247 for i in range(256)))
    (root/'corpus').write_bytes(data)
    (root/'remap').write_bytes(bytes(range(256)))
    n = len(data)+1
    (tape/'manifest').write_text(f'{n} 2 2 {n}\n')
    (tape/'ready').touch()
    (tape/'end').write_text('3\n'); (tape/'end.ready').touch()
    (tape/'window_0').write_text('2\n'); (tape/'window_2').write_text('1\n')
    payload = b''.join(struct.pack('<Q', i | (0 if i == 0 else data[i-1]) << 48) for i in range(n))
    def publish(b, contents):
        (tape/f'band_{b}.sa').write_bytes(contents)
        (tape/f'band_{b}.done').touch()
    publish(1, payload)
    env = {k:v for k,v in os.environ.items() if not k.startswith(('CROSS_', 'XSA_'))}
    env.update(CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1')
    with (root/'emit.log').open('w') as log:
        proc = subprocess.Popen([a.merge, '--binbwt', str(root/'corpus'), '--remap', str(root/'remap'),
            '--out-prefix', str(root/'frag'), '--threads', '3', '--tape-bands', str(tape),
            '--tape-emit-chunk', '1024'], stdout=log, stderr=subprocess.STDOUT, env=env)
        try:
            deadline = time.monotonic()+15
            while not (tape/'band_1.consumed').exists():
                assert proc.poll() is None, (root/'emit.log').read_text()
                assert time.monotonic() < deadline, 'completed band 1 blocked on absent band 0'
                time.sleep(.01)
            assert not (tape/'band_1.sa').exists()
            assert not (tape/'band_0.done').exists()
            publish(0, b''); publish(2, b'')
            (root/'frag.runcols.consumed').write_text('100000\n')
            assert proc.wait(timeout=15) == 0, (root/'emit.log').read_text()
            assert all((tape/f'band_{b}.consumed').exists() for b in range(3))
            print('PASS band 1 read/released before band 0 exists; rank emission and final short window complete')
        finally:
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=10)
