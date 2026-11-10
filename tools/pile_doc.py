#!/usr/bin/env python3
"""Read a Pile document out of `pile.txt` using the document map sidecars.

The map is what `xsa corpus --pile-map` produces: `seps.bin` (XSEP v1) gives each
document's start offset, `titles.bin` (XTIT v1) gives its component name
(pile_set_name) and `meta.bin`/`MANIFEST.json` describe the rest.  This tool
extracts one document's text so it can be used as a query document for
`xsa fanout`, `xsa mem` or `xsa seeds` - e.g. "take this Pile document and show
me the documents whose exact-match statistics are closest, weighted by rarity".

Usage:
    pile_doc.py <mapdir> list [--limit N] [--from I]
    pile_doc.py <mapdir> show <doc_id> [--pile PATH] [--max-bytes N]
    pile_doc.py <mapdir> find <component> [--limit N]
    pile_doc.py <mapdir> info

Formats (little-endian), both documented in xsa/src/corpus.rs:

  seps.bin    [0..4) "XSEP"  [4..8) v1  [8..12) flags  [12..16) anchor_stride=64
              [16..24) doc_count  [24..32) corpus_bytes  [32..40) anchor_count
              [40..48) base
              then doc_count * u32 deltas (d[i] = start[i] - start[i-1], d[0] = start[0] - base)
              then anchor_count * u64 absolute starts (doc k*64)

  titles.bin  [0..4) "XTIT"  [4..8) v1  [8..12) doc_count u32  [12..20) payload_bytes u64
              then payload, then doc_count * u32 offsets, then doc_count * u16 lengths

A document occupies [start[i], start[i+1] - 1): the trailing byte is the 0x1E
separator.  The last document of a LIMIT-truncated map may be cut short, so its
end is clamped to corpus_bytes.
"""
import argparse
import json
import os
import struct
import sys


def read_seps(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'XSEP' or struct.unpack_from('<I', data, 4)[0] != 1:
        raise SystemExit(f'{path}: not XSEP v1')
    stride = struct.unpack_from('<I', data, 12)[0]
    count, corpus_bytes, anchors, base = struct.unpack_from('<QQQQ', data, 16)
    if 48 + count * 4 + anchors * 8 != len(data):
        raise SystemExit(f'{path}: length disagrees with counts')
    deltas = struct.unpack_from(f'<{count}I', data, 48)
    starts, acc = [], base
    for d in deltas:          # d[i] = start[i] - start[i-1], d[0] = start[0] - base
        acc += d
        starts.append(acc)
    anchor_tab = struct.unpack_from(f'<{anchors}Q', data, 48 + count * 4)
    for k in range(0, anchors):
        if anchor_tab[k] != starts[k * stride]:
            raise SystemExit(f'anchor {k} disagrees with the delta table')
    return starts, corpus_bytes, stride


def read_titles(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:4] != b'XTIT' or struct.unpack_from('<I', data, 4)[0] != 1:
        raise SystemExit(f'{path}: not XTIT v1')
    count = struct.unpack_from('<I', data, 8)[0]
    payload = struct.unpack_from('<Q', data, 12)[0]
    offs = struct.unpack_from(f'<{count}I', data, 20 + payload)
    lens = struct.unpack_from(f'<{count}H', data, 20 + payload + count * 4)
    base = 20
    return [data[base + o:base + o + l].decode('utf-8', 'replace') for o, l in zip(offs, lens)]


def load(mapdir):
    starts, corpus_bytes, stride = read_seps(os.path.join(mapdir, 'seps.bin'))
    titles = read_titles(os.path.join(mapdir, 'titles.bin'))
    if len(titles) != len(starts):
        raise SystemExit(f'map disagrees: {len(starts)} seps vs {len(titles)} titles')
    return starts, titles, corpus_bytes


def doc_span(starts, corpus_bytes, i):
    start = starts[i]
    end = starts[i + 1] - 1 if i + 1 < len(starts) else corpus_bytes
    return start, max(start, end)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mapdir')
    ap.add_argument('cmd', choices=['list', 'show', 'find', 'info'])
    ap.add_argument('arg', nargs='?')
    ap.add_argument('--pile', default='/mnt/nvme2n1/erikg/pile.txt')
    ap.add_argument('--limit', type=int, default=20)
    ap.add_argument('--max-bytes', type=int, default=8192)
    a = ap.parse_args()

    starts, titles, corpus_bytes = load(a.mapdir)

    if a.cmd == 'info':
        man = os.path.join(a.mapdir, 'MANIFEST.json')
        counts = {}
        for t in titles:
            counts[t] = counts.get(t, 0) + 1
        print(f'map={a.mapdir}')
        print(f'documents={len(starts)} corpus_bytes={corpus_bytes}')
        if os.path.exists(man):
            m = json.load(open(man))
            for k in ('documents', 'doc_count', 'corpus_bytes', 'internal_0x1e',
                      'separators_in_range', 'covered_bytes', 'shards_used', 'pile_sha256'):
                if k in m:
                    print(f'  {k}={m[k]}')
        for name, n in sorted(counts.items(), key=lambda kv: -kv[1])[:12]:
            print(f'  {n:>10}  {name}')
        return

    if a.cmd == 'list':
        for i in range(0, min(len(starts), a.limit)):
            s, e = doc_span(starts, corpus_bytes, i)
            print(f'{i}\t{s}\t{e - s}\t{titles[i]}')
        return

    if a.cmd == 'find':
        want, shown = a.arg, 0
        for i, t in enumerate(titles):
            if t == want:
                s, e = doc_span(starts, corpus_bytes, i)
                print(f'{i}\t{s}\t{e - s}\t{t}')
                shown += 1
                if shown >= a.limit:
                    break
        return

    i = int(a.arg)
    s, e = doc_span(starts, corpus_bytes, i)
    n = min(e - s, a.max_bytes)
    with open(a.pile, 'rb') as f:
        f.seek(s)
        data = f.read(n)
    sys.stderr.write(f'doc_id={i} start={s} len={e - s} component={titles[i]}\n')
    sys.stdout.buffer.write(data)


if __name__ == '__main__':
    main()
