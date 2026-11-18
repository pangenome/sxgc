#!/usr/bin/env python3
# THE LAPTOP-CLASS THIRD ARM (operator steering 2026-11-18, AMENDMENT 4's
# "any reasonably-sized system" receipt): the 3 GB reader at a small band
# workspace with HARD walls -
#   - RAM ceiling: XSA_MEM_GUARD_MB caps total process RSS (a guard
#     refusal is a FINDING, printed with numbers, never hidden)
#   - scratch cap: --scratch-budget-bytes; the bands dir (band transients)
#     is du-asserted after the run
#   - journal: K, per-band rows/wall/RSS, the honest K x corpus rchar
# Acceptance: SS_DONE + 3,000,000,001 rows + the container BYTE-IDENTICAL
# to the default arm (pieces SHA + product SHA + row counts), all within
# the few-GB walls.
# Usage: tape_reader_laptop.py <run-root> <corpus> <baseline-bin-dir> <reader-bin> <default-arm-run>
import hashlib, json, os, subprocess, sys, time
from pathlib import Path

root, corpus, baseline, reader, default_run = map(Path, sys.argv[1:6])
corpus_bytes = corpus.stat().st_size
assert corpus_bytes == 3_000_000_000

THREADS = 8
RAM_GB = 4
SCRATCH_GB = 4
WORKSPACE = 256 * 1024 * 1024          # the operator's 128-256 MB window
RCHAR_LIMIT = 1000 * corpus_bytes      # the measurement is the number; the
                                       # backstop guards a runaway, not the proxy

remap = root / 'remap-256.bin'
run = root / 'reader'
for d in ('bands', 'merged'):
    (run / d).mkdir(parents=True, exist_ok=True)
prefix = run / 'merged' / 'frag'

common = ['--corpus', str(corpus), '--remap', str(remap),
          '--threads', str(THREADS), '--budget-bytes', str(RAM_GB << 30),
          '--memory-gb', str(RAM_GB), '--scratch-budget-bytes', str(SCRATCH_GB << 30)]
env = dict(os.environ,
           XSA_MEM_GUARD_MB=str(RAM_GB * 1024),        # THE HARD RAM CEILING
           XSA_BAND_WORKSPACE_BYTES=str(WORKSPACE),
           XSA_PARSE_PLANE_MAX=str(16 << 20),
           XSA_TAPE_RCHAR_LIMIT=str(RCHAR_LIMIT),
           CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1100')
producer = [str(reader), *common, '--out-prefix', str(prefix), '--bands', str(run / 'bands')]
finish = [str(baseline / 'xsa_finish'), '--prefix', str(prefix),
          '--corpus', str(corpus), '--remap', str(remap),
          '--output', str(run / 'product.sxi2'), '--threads', str(THREADS),
          '--poll-timeout', '0', '--wit-ram-budget', str(RAM_GB << 30),
          '--lcp-bands', str(run / 'bands'), '--no-chi']

if not remap.exists():
    import shutil
    shutil.copy(default_run / 'remap-256.bin', remap)

def fadvise_cold():
    with corpus.open('rb') as f:
        os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)

t0 = time.time()
fadvise_cold()
p_prod = subprocess.Popen(producer, env=env, stdout=open(run / 'stage-0.log', 'wb'),
                          stderr=subprocess.STDOUT)
p_fin = subprocess.Popen(finish, env=env, stdout=open(run / 'stage-1.log', 'wb'),
                         stderr=subprocess.STDOUT)
rc_prod = p_prod.wait()
rc_fin = p_fin.wait()
wall = time.time() - t0

log = (run / 'stage-0.log').read_text(errors='replace')
import re
rchar = int(re.findall(r'SS_IO rchar=(\d+)', log)[-1]) if 'SS_IO rchar=' in log else None
bands_done = len(re.findall(r'SS_BAND_DONE', log))
rss_peaks = [int(x) for x in re.findall(r'SS_BAND_DONE .*rss_kb=(\d+)', log)]
K = re.findall(r'SS_PLAN .*bands=(\d+)', log)
bands_dir_bytes = sum(f.stat().st_size for f in (run / 'bands').rglob('*') if f.is_file())

receipt = dict(arm='laptop', workspace_bytes=WORKSPACE, ram_ceiling_gb=RAM_GB,
               scratch_ceiling_gb=SCRATCH_GB, threads=THREADS,
               K=(int(K[0]) if K else None), bands_emitted=bands_done,
               exits=[rc_prod, rc_fin], ss_done=('SS_DONE' in log),
               rchar=rchar, rchar_per_corpus=(rchar / corpus_bytes if rchar else None),
               wall_s=wall, peak_band_rss_kb=max(rss_peaks) if rss_peaks else None,
               bands_dir_bytes_after=bands_dir_bytes)
print(json.dumps(receipt), flush=True)
if rc_prod != 0 or rc_fin != 0 or not receipt['ss_done']:
    print("LAPTOP_ARM_FAIL", flush=True)
    sys.exit(1)

# THE IDENTITY: the container must be byte-identical to the default arm
def sha_tree(d):
    h = hashlib.sha256()
    for f in sorted(d.rglob('*')):
        if f.is_file():
            h.update(str(f.relative_to(d)).encode())
            h.update(f.read_bytes())
    return h.hexdigest()

pieces_a = sha_tree(default_run / 'reader' / 'merged')
pieces_b = sha_tree(run / 'merged')
prod_a = hashlib.sha256((default_run / 'reader' / 'product.sxi2').read_bytes()).hexdigest()
prod_b = hashlib.sha256((run / 'product.sxi2').read_bytes()).hexdigest()
identity = dict(pieces_default=pieces_a, pieces_laptop=pieces_b,
                pieces_identical=(pieces_a == pieces_b),
                product_default=prod_a, product_laptop=prod_b,
                product_identical=(prod_a == prod_b),
                scratch_within_cap=(bands_dir_bytes <= (SCRATCH_GB << 30)))
print(json.dumps(dict(identity=identity, receipt=receipt)), flush=True)
if not (identity['pieces_identical'] and identity['product_identical']
        and identity['scratch_within_cap']):
    print("LAPTOP_ARM_IDENTITY_FAIL", flush=True)
    sys.exit(1)
print("LAPTOP_ARM_PASS", flush=True)
