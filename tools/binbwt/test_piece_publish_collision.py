#!/usr/bin/env python3
"""Reproduce a live old merge writing into a replacement run's piece directory."""
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
p.add_argument('--evidence', required=True)
a = p.parse_args()
root = Path(a.evidence)
root.mkdir(parents=True)
data = random.Random(123).randbytes(200000).translate(bytes(3+i%247 for i in range(256)))
(root/'corpus').write_bytes(data)
(root/'remap').write_bytes(bytes(range(256)))
n = len(data)+1
payload = b''.join(struct.pack('<Q', i | (0 if i == 0 else data[i-1]) << 48) for i in range(n))
env = dict(os.environ, CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='100000')
procs = []
logs = []
try:
    for generation in ['old', 'new']:
        tape = root/generation
        tape.mkdir()
        (tape/'manifest').write_text(f'{n} 2 1 {n}\n')
        (tape/'ready').touch()
        (tape/'window_0').write_text('1\n')
        (tape/'end').write_text('1\n')
        (tape/'end.ready').touch()
        log = (root/f'{generation}.log').open('w'); logs.append(log)
        proc = subprocess.Popen([a.merge, '--binbwt', str(root/'corpus'),
            '--remap', str(root/'remap'), '--out-prefix', str(root/'frag'),
            '--threads', '3', '--tape-bands', str(tape)], env=env,
            stdout=log, stderr=subprocess.STDOUT)
        procs.append(proc)
        deadline = time.monotonic()+15
        while not (root/'frag.pieces/start').exists():
            assert proc.poll() is None
            assert time.monotonic() < deadline
            time.sleep(.01)
        if generation == 'old':
            # Simulates removing/replacing scratch while the old merge lives.
            (root/'frag.pieces').rename(root/'old-pieces')
        else:
            (tape/'band_0.sa').write_bytes(payload)
            (tape/'band_0.done').touch()
            assert proc.wait(timeout=15) == 0
    (root/'old/band_0.sa').write_bytes(payload)
    (root/'old/band_0.done').touch()
    assert procs[0].wait(timeout=15) == 2
    text = (root/'old.log').read_text()
    diagnostic = next(line for line in text.splitlines() if line.startswith('CROSS_PIECE_COLLISION'))
    assert 'existed_at_start=0' in diagnostic
    assert 'piece_runs=[0,65536)' in diagnostic
    assert 'source_band=0 source_band_runs=[0,65536)' in diagnostic
    print(diagnostic)
    print('PASS replaced scratch reproduces piece collision with an ordered input stream')
finally:
    for proc in procs:
        if proc.poll() is None:
            proc.terminate(); proc.wait(timeout=10)
    for log in logs:
        log.close()
