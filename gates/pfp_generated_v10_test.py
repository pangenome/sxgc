"""Installed PFP path: compare the published product, not an input or v8 hash."""
from hashlib import sha256, shake_256
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
from overlap_v10_product_input import make

# Whole v10 reference predates the PFP handoff (receipts/build-stream/generated.json).
EXPECTED = '921f4e473e19fefcf8a7916d431c28694941323b6262ff109ae3c7c7820f19ab'


def check(xsa):
    with tempfile.TemporaryDirectory(prefix='xsa-pfp-product-') as temp:
        d = Path(temp); inputs = d/'fixture'; make(inputs)
        index = d/'index.sxi2'
        p = subprocess.run([xsa, 'build', '--pfp', inputs/'raw', '--docs-map', inputs/'map',
                            '-T', d, '-j', '1', '-v', '-o', index], capture_output=True, timeout=120)
        assert p.returncode == 0, p.stderr[-4000:]
        # The first-level words repeat, but on this input ALL upper words are
        # distinct. With ample workspace the measured census keeps P1 as leaf.
        assert re.findall(rb'PFP_LEAF level=(\d+) symbols=(\d+)',p.stderr)==[(b'1',b'101798')]
        assert b'PFP_LIFT' not in p.stderr
        counts = {k: int(v) for k, v in re.findall(rb'([a-z_]+)=(\d+)', p.stderr.split(b'PFP_ROWS ')[1].splitlines()[0])}
        # Repetitive fixture: neither visits per range nor one sort record per byte.
        assert counts[b'parse_positions'] == 101798 and counts[b'phrases'] == 2306
        assert counts[b'counted'] == counts[b'routed'] == counts[b'ordered'] == 142940
        digest = sha256(index.read_bytes()).hexdigest()
        assert digest == EXPECTED, (digest, EXPECTED)
        print('PASS published', index.stat().st_size, 'bytes', digest)


def recursive(xsa):
    # Pin made by the prior, direct P1 ranker (56dda458) BEFORE testing the
    # recursive product. Unlike the first fixture this one repeats ID blocks.
    pin = '02508df3d06b153a8211237ec3d89a3e9e0e29d57bb66e6aeaddff2b4064dcb3'
    with tempfile.TemporaryDirectory(prefix='xsa-pfp-lift-') as temp:
        d=Path(temp); raw=d/'raw'; scratch=d/'scratch'; scratch.mkdir()
        block=shake_256(b'xsa-pfp-level-lift').digest(32768).replace(b'\xff',b'\x00')
        raw.write_bytes(block*12+block[:117]); n=raw.stat().st_size
        (d/'seps.bin').write_bytes(struct.pack('<4sIII4Q3IQ', b'XSEP',1,1,64,3,n,1,0,0,117,n//2-117,0))
        cmd=[xsa,'build','--pfp',raw,'--docs-map',d,'-T',scratch,'-j','1','-v','-o',d/'index']
        team=str(min(2,len(os.sched_getaffinity(0))))
        for budget,workers in [('256M','1'),('48M',team)]:
            cmd[cmd.index('-j')+1]=workers
            p=subprocess.run(cmd+['-m',budget],capture_output=True,timeout=60)
            assert p.returncode==0,p.stderr
            assert re.findall(rb'HEAD rows=(\d+)',p.stderr)==[b'12'],p.stderr
            assert re.findall(rb'PFP_LIFT lower=(\d+) upper=(\d+)',p.stderr)==[(b'349',b'12'),(b'12725',b'349')],p.stderr
            assert sha256((d/'index').read_bytes()).hexdigest()==pin
            (d/'index').unlink()
        p=subprocess.run(cmd+['-m','1M'],capture_output=True,timeout=60)
        assert p.returncode and b'P3 direct rank needs' in p.stderr,p.stderr
        assert not (d/'index').exists() and not list(scratch.iterdir()) and not list(d.glob('.xsa-publish-*'))
        print('PASS P1=12725 -> P2=349 -> P3=12 -> 349 -> 12725, identical published v10',pin)


if __name__ == '__main__':
    xsa=Path(sys.argv[1]).resolve(); check(xsa); recursive(xsa)
