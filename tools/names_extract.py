"""names.bin — per-document extracted titles for the full pile.

Format XNAM v1 (new artifact; titles.bin/XTIT is untouched):
    [0..4)    b"XNAM"
    [4..8)    u32 version = 1
    [8..12)   u32 doc_count
    [12..20)  u64 payload_bytes
    payload:  concatenated UTF-8 titles (empty string = no title)
    u64 offsets[doc_count]   (offset into payload)
    u32 lengths[doc_count]

One sequential-ish pass over pile.txt: for each document in a RULED set, one
pread of at most HEAD bytes at its start; every other document costs no read at
all. Metadata only — never a text-sized artifact. Deterministic: no timestamps.
"""
import array
import json
import os
import struct
import sys
import time

MAP = os.environ.get('NAMES_MAP', '/mnt/nvme2n1/erikg/pilemap_full')
CORPUS = os.environ.get('NAMES_CORPUS', '/mnt/nvme2n1/erikg/pile.txt')
OUT_DIR = MAP
OUT = os.path.join(OUT_DIR, 'names.partial')
HEAD = 2048
RULED = {'Wikipedia (en)', 'ArXiv', 'PubMed Abstracts', 'StackExchange',
         'HackerNews', 'Enron Emails'}

t0 = time.time()
b = open(f'{MAP}/seps.bin', 'rb').read()
docs = struct.unpack_from('<Q', b, 16)[0]
deltas_off, anchors_off = 48, 48 + 4 * docs
deltas = array.array('I')
deltas.frombytes(b[deltas_off:deltas_off + 4 * docs])
anchors = array.array('Q')
anchors.frombytes(b[anchors_off:anchors_off + 8 * ((docs + 63) // 64)])

tb = open(f'{MAP}/titles.bin', 'rb').read()
tdocs = struct.unpack_from('<I', tb, 8)[0]
tpayload = struct.unpack_from('<Q', tb, 12)[0]
tob, tlb = 20 + tpayload, 20 + tpayload + 4 * tdocs
t_off = array.array('I'); t_off.frombytes(tb[tob:tob + 4 * tdocs])
t_len = array.array('H'); t_len.frombytes(tb[tlb:tlb + 2 * tdocs])
tpay = tb[20:20 + tpayload]

# document starts from the delta column (shift=1: delta[0] is a pad)
starts = array.array('Q', bytes(8 * docs))
s = 0
for i in range(docs):
    starts[i] = s
    s += deltas[i + 1] if i + 1 < docs else 0
corpus_len = os.path.getsize(CORPUS)
print(f'starts built: {docs:,} docs, corpus {corpus_len:,} B '
      f'({time.time() - t0:.0f}s)', flush=True)


def extract(sn, head):
    lines = head.split(b'\n')
    first = next((l for l in lines if l.strip()), b'')
    t = first.decode('utf-8', 'replace').strip()
    if sn == 'Wikipedia (en)' and 2 < len(t) <= 200:
        return t
    if sn == 'ArXiv':
        for l in lines[:30]:
            ls = l.decode('utf-8', 'replace').strip()
            if ls.startswith('title:'):
                v = ls[6:].strip().strip("'\"")
                if 3 <= len(v) <= 300 and any(c.isalpha() for c in v):
                    return v
        return None
    if sn == 'PubMed Abstracts' and 10 < len(t) <= 300:
        return t
    if sn == 'StackExchange':
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
    if sn == 'HackerNews' and ' - ' in t:
        return t.rsplit(' - ', 1)[0].strip()
    if sn == 'Enron Emails':
        for l in lines[:24]:
            ls = l.decode('utf-8', 'replace').strip()
            if ls.lower().startswith('subject:') and len(ls) > 9:
                return ls[8:].strip()
        return None
    return None


payload = bytearray()
offsets = array.array('Q')
lengths = array.array('I')
per_set = {}
fd = os.open(CORPUS, os.O_RDONLY)
t1 = time.time()
for doc in range(docs):
    dstart = starts[doc]
    dnext = starts[doc + 1] if doc + 1 < docs else corpus_len
    dlen = dnext - 1 - dstart                      # text bytes (separator at dnext-1)
    o, l = t_off[doc], t_len[doc]
    sn = tpay[o:o + l].decode()
    title = None
    if sn in RULED and dlen > 0:
        take = min(HEAD, dlen)
        title = extract(sn, os.pread(fd, take, dstart))
    offsets.append(len(payload))
    if title is not None:
        e = title.encode('utf-8', 'replace')[:1024]
        lengths.append(len(e))
        payload += e
    else:
        lengths.append(0)
    got, tot = per_set.get(sn, (0, 0))
    per_set[sn] = (got + (1 if title else 0), tot + 1)
    if (doc + 1) % 10_000_000 == 0:
        el = time.time() - t1
        print(f'  {doc + 1:,} docs, payload {len(payload):,} B, {el / 60:.0f} min '
              f'({(doc + 1) / el / 1000:.0f}k docs/s)', flush=True)
os.close(fd)

header = b'XNAM' + struct.pack('<I', 1) + struct.pack('<I', docs) \
    + struct.pack('<Q', len(payload))
with open(OUT, 'wb') as o:
    o.write(header)
    o.write(payload)
    o.write(offsets.tobytes())
    o.write(lengths.tobytes())
    o.flush()
    os.fsync(o.fileno())
os.rename(OUT, os.path.join(OUT_DIR, 'names.bin'))
named = sum(1 for v in lengths if v)
print(f'names.bin: {docs:,} docs, {named:,} titled ({100 * named / docs:.1f}%), '
      f'{len(payload):,} B payload, {(time.time() - t1) / 60:.0f} min', flush=True)
for sname in sorted(per_set):
    g, t = per_set[sname]
    print(f'  {sname:20s} {100 * g / max(1, t):5.1f}% of {t:,}')
