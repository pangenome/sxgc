#!/usr/bin/env python3
"""Read-only SXI directory preflight; never reinterpret permutation as LCP."""
import argparse
import collections
import hashlib
import json
import os
import random
import struct
import zlib


def children(text, sa, lcp, l, r, depth):
    """Reference algorithm only: caller must supply full SA, LCP and text.

    LCP[i] = lcp(T[SA[i-1]:], T[SA[i]:]); singleton terminal is END=-1.
    """
    out = []
    start = l
    for i in range(l + 1, r + 1):
        if i == r or lcp[i] <= depth:
            pos = sa[start] + depth
            label = text[pos] if pos < len(text) else -1
            out.append((label, i - start))
            start = i
    return out


def self_test():
    rng = random.Random(1701)
    text = bytes(rng.randrange(8) for _ in range(512))
    sa = sorted(range(len(text) + 1), key=lambda i: text[i:])
    lcp = [0]
    for a, b in zip(sa, sa[1:]):
        d = 0
        while a+d < len(text) and b+d < len(text) and text[a+d] == text[b+d]:
            d += 1
        lcp.append(d)
    count = 0
    for start in range(100):
        depth = 1 + start % 12
        context = text[start:start+depth]
        rows = [i for i, p in enumerate(sa) if text[p:p+depth] == context]
        assert rows == list(range(rows[0], rows[-1]+1))
        actual = dict(children(text, sa, lcp, rows[0], rows[-1]+1, depth))
        expected = collections.Counter(text[p+depth] if p+depth < len(text) else -1
                                       for p in sa[rows[0]:rows[-1]+1])
        assert actual == expected
        count += 1
    # Explicit EOF, root and singleton checks.
    for depth, l, r in [(0, 0, len(sa)), (1, sa.index(511), sa.index(511)+1)]:
        expected = collections.Counter(text[p+depth] if p+depth < len(text) else -1
                                       for p in sa[l:r])
        assert dict(children(text, sa, lcp, l, r, depth)) == expected
    return {'synthetic_contexts_exact': count, 'boundary_checks': 2,
            'real_100GB_contexts_checked': 0}


def audit(path, corpus):
    with open(path, 'rb') as f:
        header = f.read(64)
        magic, version = struct.unpack_from('<4sI', header)
        assert magic == b'SXI2' and version in (6, 7)
        n, k, runs, count, hs, length, flags, checksum, reserved = struct.unpack_from('<QQQIIQQII', header, 8)
        assert length == os.fstat(f.fileno()).st_size and hs == 64+40*count and reserved == 0
        f.seek(length-40*count if version == 7 else 64)
        directory = f.read(40*count)
        zeroed = bytearray(header)
        zeroed[56:60] = bytes(4)
        assert zlib.crc32(zeroed + directory) == checksum
        members = []
        for i in range(count):
            mid, codec, off, size, items, crc, pad = struct.unpack_from('<IIQQQII', directory, i*40)
            assert off+size <= length and pad == 0
            members.append(dict(id=mid, codec=codec, offset=off, bytes=size, count=items))
        m7 = next(m for m in members if m['id'] == 7)
        assert m7['codec'] == 7 and m7['bytes'] == 256
        f.seek(m7['offset'])
        permutation = f.read(256)
        assert sorted(permutation) == list(range(256))
        m7crc = struct.unpack_from('<IIQQQII', directory, members.index(m7)*40)[5]
        assert zlib.crc32(permutation) == m7crc
        return dict(path=path, version=version, n=n, runs=runs, bytes=length,
                    directory_crc_valid=True, members=members,
                    m7_is_256_byte_permutation=True,
                    m7_sha256=hashlib.sha256(permutation).hexdigest(),
                    corpus_path=corpus, corpus_exists=os.path.isfile(corpus),
                    persisted_full_sa=False, persisted_lcp=False,
                    measurement_status='blocked: no persisted LCP or full SA; source corpus missing' if not os.path.isfile(corpus) else 'blocked: no persisted LCP or full SA')


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('container')
    p.add_argument('corpus')
    args = p.parse_args()
    result = audit(args.container, args.corpus)
    result['reference_algorithm_checks'] = self_test()
    print(json.dumps(result, indent=2))
