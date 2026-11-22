"""Rust 1 GB recursive gate. Fresh scratch only; signal-free peer shutdown."""
import hashlib
import os
from pathlib import Path
import subprocess
import time

base = Path('/mnt/nvme2n1/erikg/TAPEREADER/climb/rung1')
corpus, remap = base / 'pile-slice1g.bin', base / 'remap-256.bin'
assert corpus.stat().st_size == 1000000665 and remap.stat().st_size == 256
r = base / f'rust_recursive_{time.time_ns()}'
r.mkdir(); (r/'bands').mkdir(); (r/'merged').mkdir()
print('arm=', r, flush=True)
binpath = Path(__file__).resolve().parents[1] / 'xsa/target/release/pfp_sort_r'
common = ['--corpus',corpus,'--remap',remap,'--threads','24']
prefix = r/'merged/frag'
env = dict(os.environ, XSA_SA_CEILING='4000000', XSA_BAND_WORKSPACE_BYTES='12000000000', SS_LCP_SLICE_WINDOW='1000000', CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1100000')
start = time.monotonic()
with (r/'run.log').open('wb') as pl, (r/'finish.log').open('wb') as fl:
    prod = subprocess.Popen([binpath,*common,'--out-prefix',prefix,'--bands',r/'bands','--memory-gb','196','--budget-bytes','64000000000','--scratch-budget-bytes',str(2*corpus.stat().st_size)],stdout=pl,stderr=pl,env=env)
    fin = subprocess.Popen(['/tmp/xsa_finish_cur',*common,'--prefix',prefix,'--lcp-bands',r/'bands','--output',r/'product.sxi2','--poll-timeout','0','--wit-ram-budget','210453397504','--with-chi','--container-stream'],stdout=fl,stderr=fl)
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
assert h.hexdigest() == 'b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3'
subprocess.run(['cmp',r/'product.sxi2',base/'run_wide/product.sxi2'],check=True)
