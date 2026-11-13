"""names.bin: per-document extracted titles for the full pile.

One sequential pass over pile.txt (idle I/O priority), order-preserving and
deterministic. Output format = XTIT v1 (same as titles.bin): payload, then
u32 offsets, then u16 lengths. Metadata only — the text is never materialized;
each document contributes at most a short title string.

For sets with no extractable title (Pile-CC, OpenWebText2, Github, ...), the
entry is EMPTY (length 0): callers fall back to titles.bin's pile_set_name.
"""
import array
import json
import os
import struct
import sys
import time

MAP = '/mnt/nvme2n1/erikg/pilemap_full'
CORPUS = '/mnt/nvme2n1/erikg/pile.txt'
OUT_DIR = '/mnt/nvme2n1/erikg/pilemap_full'
OUT = os.path.join(OUT_DIR, 'names.partial')
HEAD = 2048
CHUNK = 1 << 24

b = open(f'{MAP}/seps.bin', 'rb').read()
docs = struct.unpack_from('<Q', b, 16)[0]
tpay = struct.unpack_from('<Q', b, 8 + 16)[0] if False else None
deltas_off, anchors_off = 48, 48 + 4 * docs
deltas = array.array('I')
deltas.frombytes(b[deltas_off:deltas_off + 4 * docs])
anchors = array.array('Q')
anchors.frombytes(b[anchors_off:anchors_off + 8 * ((docs + 63) // 64)])

tb = open(f'{MAP}/titles.bin', 'rb').read()
tdocs = struct.unpack_from('<I', tb, 8)[0]
tpayload = struct.unpack_from('<Q', tb, 12)[0]
tob, tlb = 20 + tpayload, 20 + tpayload + 4 * tdocs
t_off = array.array('I')
t_off.frombytes(tb[tob:tob + 4 * tdocs])
t_len = array.array('H')
t_len.frombytes(tb[tlb:tlb + 2 * tdocs])
pay = tb[20:20 + tpayload]

def setname(d):
    o, l = t_off[d], t_len[d]
    return pay[o:o + l].decode()

def extract(s, head):
    lines = head.split(b'\n')
    first = next((l for l in lines if l.strip()), b'')
    t = first.decode('utf-8', 'replace').strip()
    if s == 'Wikipedia (en)' and 2 < len(t) <= 200:
        return t
    if s == 'ArXiv':
        for l in lines[:30]:
            ls = l.decode('utf-8', 'replace').strip()
            if ls.startswith('title:'):
                v = ls[6:].strip().strip("'\"")
                if 3 <= len(v) <= 300 and any(c.isalpha() for c in v):
                    return v
        return None
    if s == 'PubMed Abstracts' and 10 < len(t) <= 300:
        return t
    if s == 'StackExchange':
        seen_q = False
        for l in lines[:6]:
            ls = l.decode('utf-8', 'replace').strip()
            if not ls:
                continue
            if not seen_q and ls == 'Q:':
                seen_q = True
                continue
            if seen_q and 3 <= len(ls) <= 240:
                return ls
        return None
    if s == 'HackerNews' and ' - ' in t:
        return t.rsplit(' - ', 1)[0].strip()
    if s == 'Enron Emails':
        for l in lines[:24]:
            ls = l.decode('utf-8', 'replace').strip()
            if ls.lower().startswith('subject:') and len(ls) > 9:
                return ls[8:].strip()
        return None
    return None

# document starts, one pass over the deltas
t0 = time.time()
starts = array.array('Q', [0]) * docs
s = 0
for i in range(docs):
    starts[i] = s
    s += deltas[i + 1] if i + 1 < docs else 0
corpus_len = s + deltas[docs - 1] if docs else 0
# last doc length: to the corpus end (record text + separator)
corpus_len = os.path.getsize(CORPUS)
print(f"starts built: {docs:,} docs, corpus {corpus_len:,} B "
      f"(walk {(time.time()-t0):.0f}s)", flush=True)

RULED = {'Wikipedia (en)', 'ArXiv', 'PubMed Abstracts', 'StackExchange',
         'HackerNews', 'Enron Emails'}

payload = bytearray()
offsets = array.array('I')
lengths = array.array('H')
stats = {}
f = open(CORPUS, 'rb')
doc = 0
buf = b''
buf_start = 0  # file offset of buf[0]
pos = 0
per_set = {}
t0 = time.time()
while doc < docs:
    dstart = starts[doc]
    dnext = starts[doc + 1] if doc + 1 < docs else corpus_len
    dlen = dnext - 1 - dstart  # text bytes (separator at dnext-1)
    take = min(HEAD, dlen)
    # read until the file cursor has covered the head region
    while pos < dstart + take:
        c = f.read(CHUNK)
        if not c:
            break
        buf += c
        pos += len(c)
    # drop bytes of earlier docs still in buf
    skip = dstart - buf_start
    if skip > 0:
        if skip >= len(buf):
            buf = b''
        else:
            buf = buf[skip:]
        buf_start = dstart
    sn = setname(doc)
    title = extract(sn, buf[:take]) if sn in RULED else None
    if title is not None:
        e = title.encode('utf-8', 'replace')[:1024]
        offsets.append(len(payload))
        lengths.append(len(e))
        payload += e
    else:
        offsets.append(len(payload))
        lengths.append(0)
    got, tot = per_set.get(sn, (0, 0))
    per_set[sn] = (got + (1 if title else 0), tot + 1)
    # advance past the remainder of this document
    if dlen > take:
        rest = dlen - take
        if len(buf) >= rest + take:
            buf = buf[take + rest:]
            buf_start = dstart + dlen
        else:
            f.seek(dstart + dlen)
            pos = dstart + dlen
            buf = b''
            buf_start = dstart + dlen
    else:
        buf = buf[take:]
        buf_start = dstart + take
    doc += 1
    if doc % 10_000_000 == 0:
        print(f"  {doc:,} docs, payload {len(payload):,} B, "
              f"{(time.time()-t0)/60:.0f} min", flush=True)

header = b'XTIT' + struct.pack('<I', 1) + struct.pack('<I', docs) \
    + struct.pack('<Q', len(payload))
with open(OUT, 'wb') as o:
    o.write(header)
    o.write(payload)
    o.write(offsets.tobytes())
    o.write(lengths.tobytes())
    o.flush()
    os.fsync(o.fileno())
os.rename(OUT, os.path.join(OUT_DIR, 'names.bin'))
n_named = sum(1 for l in lengths if l > 0)
print(f"names.bin written: {docs:,} docs, {n_named:,} titled "
      f"({100*n_named/docs:.1f}%), {len(payload):,} B payload", flush=True)
for sname in sorted(per_set):
    g, t = per_set[sname]
    print(f"  {sname:20s} {100*g/max(1,t):5.1f}% of {t:,}")
