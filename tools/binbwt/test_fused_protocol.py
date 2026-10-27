#!/usr/bin/env python3
"""Exercise leading/trailing empty bands and K=1 with an existing tiny oracle.

Reads the oracle in bounded blocks. Never writes a concatenated rank stream.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
for name in ('bundle', 'corpus', 'remap', 'sa', 'column', 'isa-sample', 'sha-expect'):
    p.add_argument('--'+name, required=True)
a = p.parse_args()
n = Path(a.corpus).stat().st_size
chunk = 4096
bands = (n+chunk-1)//chunk+2
clean = {k:v for k,v in os.environ.items() if not k.startswith(('CROSS_', 'XSA_FINISH_'))}
with tempfile.TemporaryDirectory(prefix='fused-protocol-') as d:
    root = Path(d)
    tape = root/'tape'; tape.mkdir()
    (tape/'manifest').write_text(f'{n} 1\n')
    (tape/'end').write_text(f'{bands}\n')
    (tape/'end.ready').touch()
    (tape/'ready').touch()
    prefix = root/'frag'
    with (root/'emit.log').open('w') as log:
        proc = subprocess.Popen([str(Path(a.bundle)/'cross_lcp_merge'), '--binbwt', a.corpus,
                '--remap', a.remap, '--out-prefix', str(prefix), '--threads', '8',
                '--tape-bands', str(tape)], stdout=log, stderr=subprocess.STDOUT,
                env=dict(clean, CROSS_V2_PIECE_WINDOW='100000000'))
        try:
            with open(a.sa, 'rb') as sa, open(a.sa+'.sym', 'rb') as syms:
                sa.seek(48)
                for b in range(bands):
                    packed = b''
                    if b not in (0, bands-1):
                        raw, sym = sa.read(chunk*8), syms.read(chunk)
                        assert len(raw) == 8*len(sym)
                        packed = b''.join(struct.pack('<Q', pos | c<<48)
                                         for (pos,),c in zip(struct.iter_unpack('<Q', raw), sym))
                    (tape/f'band_{b}.sa').write_bytes(packed)
                    (tape/f'band_{b}.done').touch()
                    deadline = time.monotonic()+30
                    while not (tape/f'band_{b}.consumed').exists():
                        assert proc.poll() is None, (proc.returncode,(root/'emit.log').read_text())
                        assert time.monotonic()<deadline, 'consumption timed out'
                        time.sleep(.001)
                    assert not (tape/f'band_{b}.sa').exists()
            assert proc.wait(timeout=30)==0
        finally:
            if proc.poll() is None: proc.kill(); proc.wait()
    with (root/'finish.log').open('w') as log:
        subprocess.run([str(Path(a.bundle)/'xsa_finish'), '--prefix', str(prefix), '--corpus', a.corpus,
            '--remap', a.remap, '--output', str(root/'product.sxi2'), '--threads', '8',
            '--lcp-column', a.column, '--isa-sample', a.isa_sample], check=True,
            stdout=log, stderr=subprocess.STDOUT, env=clean)
    got = hashlib.sha256((root/'product.sxi2').read_bytes()).hexdigest()
    assert got == a.sha_expect, got
    print(json.dumps(dict(passed=True, n=n, bands=bands, empty_bands=[0,bands-1], K=1,
                         max_band_rows=chunk, sha256=got, trailing_empty_acknowledged=True),indent=2))
