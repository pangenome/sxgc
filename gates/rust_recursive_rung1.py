"""Rust recursive ladder (default 1 GB; --rung 2 selects 3 GB). Fresh scratch only."""
import argparse
import hashlib
import struct
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument('--rung', type=int, choices=[1,2], default=1)
rung = parser.parse_args().rung
root = Path('/mnt/nvme2n1/erikg/TAPEREADER')
base = root / 'climb' / f'rung{rung}'
if rung == 1:
    corpus, remap = base / 'pile-slice1g.bin', base / 'remap-256.bin'
    finish = Path('/tmp/xsa_finish_cur'); finish_flags = ['--with-chi','--container-stream']
    anchor = base/'run_wide/product.sxi2'
else:
    corpus, remap = root.parent/'KEYRADIX/c3g.txt', root/'bins-p1/run14/remap-256.bin'
    finish = root/'bins-p1/bin/xsa_finish'; finish_flags = []
    anchor = root/'bins-p1/run14e/reader/product.sxi2'
assert corpus.stat().st_size == (1000000665 if rung == 1 else 3000000000) and remap.stat().st_size == 256
assert finish.is_file() and anchor.is_file()
r = base / f'rust_recursive_{time.time_ns()}'
r.mkdir(); (r/'bands').mkdir(); (r/'merged').mkdir()
print('arm=', r, flush=True)
binpath = Path(__file__).resolve().parents[1] / 'xsa/target/release/pfp_sort_r'
common = ['--corpus',corpus,'--remap',remap,'--threads','24']
prefix = r/'merged/frag'
env = dict(os.environ, XSA_SA_CEILING=('4000000' if rung == 1 else '12000000'), XSA_BAND_WORKSPACE_BYTES='12000000000', SS_LCP_SLICE_WINDOW='1000000', CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1100000')
start = time.monotonic()
with (r/'run.log').open('wb') as pl, (r/'finish.log').open('wb') as fl:
    prod = subprocess.Popen([binpath,*common,'--out-prefix',prefix,'--bands',r/'bands','--memory-gb','196','--budget-bytes','64000000000','--scratch-budget-bytes',str(2*corpus.stat().st_size)],stdout=pl,stderr=pl,env=env)
    fin = subprocess.Popen([finish,*common,'--prefix',prefix,'--lcp-bands',r/'bands','--output',r/'product.sxi2','--poll-timeout','0','--wit-ram-budget','210453397504',*finish_flags],stdout=fl,stderr=fl)
    while True:
        pr, fr = prod.poll(), fin.poll()
        if pr is not None and fr is not None: break
        if pr not in (None, 0) and fr is None: (r/'stop.requested').touch(exist_ok=True)
        if fr not in (None, 0) and pr is None and (r/'merged/modeb').is_dir(): (r/'merged/modeb/stop.requested').touch(exist_ok=True)
        time.sleep(.5)
print(f'producer_rc={pr} finish_rc={fr} wall_s={time.monotonic()-start:.2f}', flush=True)
assert pr == 0 and fr == 0
assert 'kernel=induced' in (r/'run.log').read_text()
h = hashlib.sha256()
with (r/'product.sxi2').open('rb') as f:
    for block in iter(lambda: f.read(1<<20), b''): h.update(block)
print('sha256='+h.hexdigest(), flush=True)
if rung == 1:
    assert h.hexdigest() == 'b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3'
    subprocess.run(['cmp',r/'product.sxi2',anchor],check=True)
else:
    def members(f):
        header = f.read(64)
        assert header[:4] == b'SXI2' and struct.unpack_from('<I', header, 32)[0] == 9
        end = f.seek(-9*40, 2)
        result = {}
        for _ in range(9):
            mid, codec, off, n, count = struct.unpack_from('<IIQQQ', f.read(40))
            assert mid not in result and 64 <= off <= off+n <= end
            result[mid] = (codec, off, n, count)
        return result
    with (r/'product.sxi2').open('rb') as a, anchor.open('rb') as b:
        ma, mb = members(a), members(b)
        assert ma.keys() == mb.keys()
        for mid in sorted(ma):
            codec, oa, size, count = ma[mid]; cb, ob, nb, cntb = mb[mid]
            assert (codec,size,count) == (cb,nb,cntb), f'metadata mismatch m{mid}'
            a.seek(oa); b.seek(ob)
            for pos in range(0,size,1<<20):
                n = min(1<<20,size-pos); x,y = a.read(n),b.read(n)
                if x != y: raise ValueError(f'm{mid} differs at {pos + next(i for i in range(n) if x[i]!=y[i])}')
            print(f'm{mid} bytes={size} identical',flush=True)
