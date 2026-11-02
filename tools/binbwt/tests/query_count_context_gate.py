#!/usr/bin/env python3
"""100 GB count regression and document-context byte oracle. No third-party modules."""
import argparse, codecs, json, mmap, pathlib, random, re, struct, subprocess, tempfile
p = argparse.ArgumentParser()
p.add_argument('xsa'); p.add_argument('container'); p.add_argument('docs')
a = p.parse_args()
def query(*args):
    r = subprocess.run([a.xsa, 'query', a.container, '-j', '32', *args], capture_output=True)
    if r.returncode:
        raise RuntimeError(r.stderr.decode(errors='replace'))
    n = int(re.search(rb'QLITE_OPEN .*? n=(\d+)', r.stderr)[1])
    return r, n
fixture = pathlib.Path(__file__).parent/'fixtures/count-failure.patterns'
r, n = query('--count', '--patterns', str(fixture))
counts = [int(row.rsplit(b'\t', 1)[1]) for row in r.stdout.split(b'\n') if row]
assert counts == [0, 0], counts
rng = random.Random(20261102)
alphabet = [b for b in range(256) if b != 30]
patterns = [bytes(rng.choices(alphabet, k=size)) for size in [32, 128, 500, 1024, 65536] for _ in range(20)]
patterns += [bytes([b])*500 for b in [0, 255]]
with tempfile.NamedTemporaryFile() as f:
    f.write(b'\x1e'.join(patterns)+b'\x1e'); f.flush()
    r, n = query('--count', '--patterns', f.name)
assert int(re.search(rb'QUERY_ABSENT_SYMBOL patterns=(\d+)', r.stderr)[1]) > 0
counts = [int(row.rsplit(b'\t', 1)[1]) for row in r.stdout.split(b'\n') if row]
assert len(counts) == len(patterns)
assert all(c == 0 and c <= n for c in counts), counts
print(f'U1 repro counts=0,0; absent_batch={len(counts)}/{len(counts)} zero; max_count={max(counts)} <= n={n}', flush=True)
r, n = query('--docs', a.docs, '--context', '120', '--output', 'jsonl', 'domestic cat')
row = json.loads(r.stdout)
assert len(row['offsets']) == len(row['contexts']) == row['count']
assert int(re.search(rb'QUERY_CONTEXT window=120 clamped_hits=(\d+)', r.stderr)[1]) > 0
with open(pathlib.Path(a.docs)/'seps.bin', 'rb') as f:
    seps = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    count = struct.unpack_from('<Q', seps, 16)[0]
    # Find document starts with the sidecar's sparse anchors.
    def bounds(pos):
        lo, hi = 0, (count+63)//64
        def anchor(i): return struct.unpack_from('<Q', seps, 48+count*4+i*8)[0]
        while lo < hi:
            mid = (lo+hi)//2
            if anchor(mid) <= pos: lo = mid+1
            else: hi = mid
        doc = (lo-1)*64; start = anchor(lo-1)
        while doc+1 < count:
            nxt = start + struct.unpack_from('<I', seps, 48+(doc+1)*4)[0]
            if nxt > pos: return start, nxt
            start = nxt; doc += 1
        return start, n-1
    with open(pathlib.Path(a.docs)/'corpus.bin', 'rb') as corpus:
        for pos, ctx in zip(row['offsets'], row['contexts']):
            start, end = bounds(pos)
            corpus.seek(end-1)
            if corpus.read(1) == b'\x1e': end -= 1
            assert ctx['start'] == max(start, pos-120)
            assert ctx['end'] == min(end, pos+120)
            corpus.seek(ctx['start']); raw = corpus.read(ctx['end']-ctx['start'])
            assert b'\x1e' not in raw
            # Decode the documented lossless byte escaping.
            assert codecs.escape_decode(ctx['snippet'].encode('ascii'))[0] == raw
        assert len(row['offsets']) >= 3
        for i in [0, len(row['offsets'])//2, len(row['offsets'])-1]:
            pos = row['offsets'][i]; corpus.seek(pos)
            assert corpus.read(12) == b'domestic cat'
            print(f'U3 spot-check pos={pos} snippet={row["contexts"][i]["snippet"]!r}', flush=True)
print(f'U3 --context 120 domestic cat: {len(row["offsets"])}/{len(row["offsets"])} snippets inside document; no 0x1E; starts <=120 bytes before hit; corpus spot-checks=3/3', flush=True)
print(next(line.decode() for line in r.stderr.splitlines() if line.startswith(b'QUERY_CONTEXT')), flush=True)
