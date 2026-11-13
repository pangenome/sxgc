"""names pilot: per-set extractability of a document title from the text's own head.

Sampled over the 100 GB head. Output: per-set extraction rate + examples +
weighted coverage. Metadata only — nothing text-sized is materialized.
"""
import json
import random
import struct

MAP = '/mnt/nvme2n1/erikg/pilemap100'
CORPUS = '/mnt/nvme2n1/erikg/pile-head100.txt'

# --- seps.bin walk (shift=1: delta(0) is a pad) ---
b = open(f'{MAP}/seps.bin', 'rb').read()
docs = struct.unpack_from('<Q', b, 16)[0]
deltas_off, anchors_off = 48, 48 + 4 * docs
def docstart(d):
    a = d // 64
    s = struct.unpack_from('<Q', b, anchors_off + 8 * a)[0]
    for j in range(a * 64 + 1, d + 1):
        s += struct.unpack_from('<I', b, deltas_off + 4 * j)[0]
    return s

# --- titles.bin: the pile_set_name per doc + per-set doc counts ---
tb = open(f'{MAP}/titles.bin', 'rb').read()
tdocs = struct.unpack_from('<I', tb, 8)[0]
tpay = struct.unpack_from('<Q', tb, 12)[0]
tob, tlb = 20 + tpay, 20 + tpay + 4 * tdocs
def setname(d):
    o = struct.unpack_from('<I', tb, tob + 4 * d)[0]
    l = struct.unpack_from('<H', tb, tlb + 2 * d)[0]
    return tb[20 + o:20 + o + l].decode()
by_set = {}
for d in range(tdocs):
    by_set.setdefault(setname(d), []).append(d)

# --- extraction rules (head bytes only) ---
def extract(s, head):
    lines = head.split(b'\n')
    first = next((l for l in lines if l.strip()), b'')
    t = first.decode('utf-8', 'replace').strip()
    if s == 'Wikipedia (en)' and 2 < len(t) <= 200:
        return t
    if s == 'ArXiv':
        for l in lines[:12]:
            ls = l.decode('utf-8', 'replace').strip()
            if ls.startswith('title:'):
                return ls[6:].strip().strip("'\"")
        return None
    if s in ('PubMed Abstracts',) and 10 < len(t) <= 300:
        return t
    if s == 'StackExchange' and t.startswith('Q:'):
        return t[2:].strip() or None
    if s == 'HackerNews' and ' - ' in t:
        return t.rsplit(' - ', 1)[0].strip()
    return None

random.seed(20261113)
F = open(CORPUS, 'rb')
rows = []
total_docs = 0
cov_num = cov_den = 0
for s, ids in sorted(by_set.items()):
    n = len(ids)
    total_docs += n
    sample = random.sample(ids, min(2000, n))
    got = 0
    ex = []
    for d in sample:
        st = docstart(d)
        nxt = docstart(d + 1) if d + 1 < docs else st + 1
        avail = min(512, max(0, nxt - 1 - st))
        if avail <= 0:
            continue
        F.seek(st)
        t = extract(s, F.read(avail))
        if t:
            got += 1
            if len(ex) < 2:
                ex.append(t[:60])
    rate = got / max(1, len(sample))
    cov_num += rate * n
    cov_den += n
    rows.append((s, n, got, len(sample), ex))

for s, n, got, k, ex in rows:
    pct = 100 * got / max(1, k)
    print(f"{s:20s} docs={n:8d}  extracted {pct:5.1f}%  e.g. {ex}")
print(f"\nweighted coverage over {total_docs:,} docs: {100*cov_num/cov_den:.1f}%")
print("fallback for the rest: pile_set_name (as today)")
