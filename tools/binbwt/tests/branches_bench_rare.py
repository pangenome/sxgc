#!/usr/bin/env python3
"""RARE-ONLY branch benchmark: the LM's real workload (context width <= 2).

The mixed benchmark (branches_bench.py) is 90% corpus samples + 10% common
English, and the common contexts are WIDE: their interval can be millions of
rows. A language model asks the other question - "what can follow THIS 48-byte
context I am actually holding", i.e. contexts whose interval is one or two rows.
This gate times exactly those, separately, on the same container.

Two phases against one --serve process:
  probe  - send 1.2x --count sampled 48-byte contexts UNTIMED, keep the ones
           whose response says width <= --max-width (default 2);
  timed  - 1000 warmup requests, then --count of the kept contexts.
Every reply is checked for frame order and for the closed count invariant
sum(branches) + other + eof == width, exactly like the mixed benchmark.
"""
import argparse, json, mmap, pathlib, random, struct, subprocess, threading, time

p = argparse.ArgumentParser()
p.add_argument('xsa'); p.add_argument('container'); p.add_argument('corpus')
p.add_argument('--jobs', type=int, required=True)
p.add_argument('--out', required=True)
p.add_argument('--count', type=int, default=100000)
p.add_argument('--max-width', type=int, default=2)
a = p.parse_args()
out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
f = open(a.corpus, 'rb'); data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
rng = random.Random(20261104)
# 1.2x candidates, so the kept set reaches --count with room to spare.
cands = [data[rng.randrange(len(data) - 48):][:48] for _ in range(int(a.count * 1.2))]
log = open(out / f'j{a.jobs}.stderr', 'wb')
proc = subprocess.Popen([a.xsa, 'branches', a.container, '--serve', '-j', str(a.jobs), '-k', '8'],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
ready = threading.Event()
def stderr():
    for line in proc.stderr:
        log.write(line); log.flush()
        if b'BRANCH_READY' in line: ready.set()
threading.Thread(target=stderr, daemon=True).start()
while not ready.wait(1):
    if proc.poll() is not None: raise RuntimeError('server failed during startup')
def exact(n):
    b = b''
    while len(b) < n:
        chunk = proc.stdout.read(n - len(b))
        if not chunk: raise EOFError()
        b += chunk
    return b
def response(id):
    size, = struct.unpack('<I', exact(4)); b = exact(size)
    got, l, r, width, eof, other, n = struct.unpack_from('<6QH', b)
    assert got == id and r - l == width and size == 50 + 9 * n
    assert sum(struct.unpack_from('<BQ', b, 50 + i * 9)[1] for i in range(n)) + eof + other == width
    return width, eof, other
def send(ps, close=False):
    for pat in ps: proc.stdin.write(struct.pack('<I', len(pat)) + pat)
    proc.stdin.flush()
    if close: proc.stdin.close()

# Phase 1: probe, untimed.
t = threading.Thread(target=send, args=(cands,)); t.start()
kept = []
for i in range(len(cands)):
    width, eof, other = response(i)
    if width <= a.max_width: kept.append(cands[i])
t.join()
kept = kept[:a.count]
assert len(kept) == a.count, f'only {len(kept)} contexts of width <= {a.max_width}; raise the candidate multiple'
print(f'probe: {len(kept)}/{len(cands)} sampled 48-byte contexts have width <= {a.max_width}', flush=True)

# Phase 2: 1000 warmup requests, then the timed run.
base = len(cands)
warm = kept[:1000]
t = threading.Thread(target=send, args=(warm,)); t.start()
for i in range(len(warm)): response(base + i)
t.join(); print(f'warm j={a.jobs} complete', flush=True)
start = time.perf_counter(); t = threading.Thread(target=send, args=(kept, True)); t.start()
for i in range(len(kept)):
    response(base + len(warm) + i)
    if i and i % 10000 == 0: print(f'j={a.jobs} {i}/{len(kept)}', flush=True)
t.join(); elapsed = time.perf_counter() - start
assert proc.wait() == 0
result = {'jobs': a.jobs, 'contexts': len(kept), 'seconds': elapsed,
          'contexts_per_s': len(kept) / elapsed,
          'mix': f'sampled 48-byte contexts with width <= {a.max_width} only; seed 20261104',
          'warmup': len(warm)}
(out / f'j{a.jobs}.json').write_text(json.dumps(result, indent=2) + '\n')
print(f'Z3 RARE -j {a.jobs}: {len(kept)/elapsed:.3f} contexts/s '
      f'({len(kept)} contexts of width <= {a.max_width}, warm cache, k=8)', flush=True)
