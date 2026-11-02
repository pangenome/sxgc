#!/usr/bin/env python3
"""Independent document-output oracle; fixture titles are each record's first line.

prepare CORPUS DIR creates sidecars for the existing 150 MB, 0x1E fixture.
check DIR PLAIN_TSV DOCS_TSV verifies every position/title with full delta decode.
top DIR DOCS_TSV verifies documents independently and prints the top five.
"""
import bisect
import collections
import json
import pathlib
import struct
import sys


def prepare(corpus, out):
    data = pathlib.Path(corpus).read_bytes()
    assert data.endswith(b'\x1e')
    records = data[:-1].split(b'\x1e')
    starts, titles, offset = [], [], 0
    for record in records:
        starts.append(offset)
        titles.append(record.split(b'\n', 1)[0][:65535])
        offset += len(record) + 1
    assert offset == len(data)
    n = len(starts)
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=False)
    seps = bytearray(struct.pack('<4sIIIQQQQ', b'XSEP', 1, 1, 64, n, len(data), (n+63)//64, 0))
    prev = 0
    for start in starts:
        seps.extend(struct.pack('<I', start-prev))
        prev = start
    for start in starts[::64]:
        seps.extend(struct.pack('<Q', start))
    payload = b''.join(titles)
    title_data = bytearray(struct.pack('<4sIIQ', b'XTIT', 1, n, len(payload)) + payload)
    offset = 0
    for title in titles:
        title_data.extend(struct.pack('<I', offset))
        offset += len(title)
    for title in titles:
        title_data.extend(struct.pack('<H', len(title)))
    (out/'seps.bin').write_bytes(seps)
    (out/'titles.bin').write_bytes(title_data)
    (out/'MANIFEST.json').write_text(json.dumps(dict(doc_count=n, corpus_bytes=len(data),
        fixture_source=str(corpus), title_policy='record first line, at most 65535 bytes'))+'\n')
    print(f'PREPARED docs={n} corpus_bytes={len(data)}')


def decode(directory):
    directory = pathlib.Path(directory)
    s = (directory/'seps.bin').read_bytes()
    magic, version, flags, stride, count, size, anchors, base = struct.unpack_from('<4sIIIQQQQ', s)
    assert (magic, version, stride) == (b'XSEP', 1, 64)
    # Deliberately do not use the query reader's anchor algorithm.
    starts, pos = [], base
    for (delta,) in struct.iter_unpack('<I', s[48:48+count*4]):
        pos += delta
        starts.append(pos)
    t = (directory/'titles.bin').read_bytes()
    magic, version, title_count, payload = struct.unpack_from('<4sIIQ', t)
    assert (magic, version, title_count) == (b'XTIT', 1, count)
    offsets = struct.unpack_from(f'<{count}I', t, 20+payload)
    lengths = struct.unpack_from(f'<{count}H', t, 20+payload+count*4)
    return starts, t, offsets, lengths


def check(directory, output, plain=None):
    starts, titles, offsets, lengths = decode(directory)
    hits, counts, names = [], collections.Counter(), {}
    for row in pathlib.Path(output).read_bytes().split(b'\n')[:-1]:
        pid, doc, local, rest = row.split(b'\t', 3)
        title, global_offset = rest.rsplit(b'\t', 1)
        pid, doc, local, pos = map(int, (pid, doc, local, global_offset))
        expected_doc = bisect.bisect_right(starts, pos)-1
        raw_title = titles[20+offsets[expected_doc]:20+offsets[expected_doc]+lengths[expected_doc]]
        escaped = raw_title.replace(b'\\', b'\\\\').replace(b'\n', b'\\n')
        assert (doc, local, title) == (expected_doc, pos-starts[expected_doc], escaped)
        hits.append((pid, pos))
        counts[doc] += 1
        names[doc] = title
    if plain:
        expected = [tuple(map(int, line.split(b'\t'))) for line in pathlib.Path(plain).read_bytes().splitlines()]
        assert sorted(hits) == sorted(expected)
    print(f'DOCS_ORACLE patterns={len(set(p for p, _ in hits))} hits={len(hits)} positions_and_titles=identical')
    if not plain:
        print('doc_id\toccurrences\ttitle')
        for doc, count in sorted(counts.items(), key=lambda pair: (-pair[1], pair[0]))[:5]:
            print(f'{doc}\t{count}\t'+names[doc].decode('utf-8', errors='backslashreplace'))


if __name__ == '__main__':
    mode, *args = sys.argv[1:]
    if mode == 'prepare':
        prepare(*args)
    elif mode == 'check':
        directory, plain, output = args
        check(directory, output, plain)
    elif mode == 'top':
        check(*args)
    else:
        raise SystemExit(__doc__)
