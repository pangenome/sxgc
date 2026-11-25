"""Installed PFP path: compare the published product, not an input or v8 hash."""
from hashlib import sha256
from pathlib import Path
import re
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
        p = subprocess.run([xsa, 'build-stream', inputs/'raw', '--docs-map', inputs/'map',
                            '-T', d, '-j', '1', '-v', '-o', index], capture_output=True, timeout=120)
        assert p.returncode == 0, p.stderr[-4000:]
        counts = {k: int(v) for k, v in re.findall(rb'([a-z_]+)=(\d+)', p.stderr.split(b'PFP_ROWS ')[1].splitlines()[0])}
        # Repetitive fixture: neither visits per range nor one sort record per byte.
        assert counts[b'parse_positions'] == 101798 and counts[b'phrases'] == 2306
        assert counts[b'counted'] == counts[b'routed'] == counts[b'ordered'] == 142940
        digest = sha256(index.read_bytes()).hexdigest()
        assert digest == EXPECTED, (digest, EXPECTED)
        print('PASS published', index.stat().st_size, 'bytes', digest)


if __name__ == '__main__': check(Path(sys.argv[1]).resolve())
