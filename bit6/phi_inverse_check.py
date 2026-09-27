#!/usr/bin/env python3
"""Bounded-memory validation; baselines are used only as external oracles."""
import argparse
import os
import pathlib
import struct
import numpy as np

p = argparse.ArgumentParser()
sub = p.add_subparsers(dest='mode', required=True)
q = sub.add_parser('column'); q.add_argument('agg'); q.add_argument('heads')
q = sub.add_parser('sets'); q.add_argument('actual'); q.add_argument('expected'); q.add_argument('count', type=int); q.add_argument('scratch')
q = sub.add_parser('anchors'); q.add_argument('ri4'); q.add_argument('heads'); q.add_argument('anchors')
a = p.parse_args()
CHUNK = 1 << 20
if a.mode == 'column':
    with open(a.agg, 'rb') as f, open(a.heads, 'rb') as h:
        magic, runs = struct.unpack('<IQ', f.read(12))
        assert magic == 0x31415243
        assert os.path.getsize(a.agg) == 12 + 32*runs
        assert os.path.getsize(a.heads) == 8*runs
        f.seek(12+8*runs)
        for start in range(0, runs, CHUNK):
            size = 8*min(CHUNK, runs-start)
            x, y = f.read(size), h.read(size)
            if x != y:
                xx, yy = np.frombuffer(x, '<u8'), np.frombuffer(y, '<u8')
                k = int(np.flatnonzero(xx != yy)[0])
                raise AssertionError(f'head mismatch run={start+k} baseline={xx[k]} actual={yy[k]}')
        print(f'PASS saFirst byte identity runs={runs} bytes={8*runs}', flush=True)
elif a.mode == 'sets':
    scratch = pathlib.Path(a.scratch); scratch.mkdir(parents=True, exist_ok=True)
    arrays = []
    for label, name in [('actual', a.actual), ('expected', a.expected)]:
        assert os.path.getsize(name) == a.count*8
        # Sort writable scratch copies, never mutate ground truth.
        out = scratch / (label+'.sorted.u64')
        assert not out.exists(), f'refuse overwrite: {out}'
        source = np.memmap(name, dtype='<u8', mode='r')
        dest = np.memmap(out, dtype='<u8', mode='w+', shape=(a.count,))
        for i in range(0, a.count, CHUNK): dest[i:i+CHUNK] = source[i:i+CHUNK]
        del source
        dest.sort(kind='quicksort'); dest.flush(); arrays.append(dest)
        print(f'SORTED {label} count={a.count}', flush=True)
    for i in range(0, a.count, CHUNK):
        assert np.array_equal(arrays[0][i:i+CHUNK], arrays[1][i:i+CHUNK]), f'sorted mismatch at chunk {i}'
    print(f'PASS numpy sorted-set equality=True count={a.count} (multiset equality also checked)', flush=True)
else:
    with open(a.ri4, 'rb') as f:
        magic, version, n, k, runs = struct.unpack('<IIQQQ', f.read(32))
        assert (magic, version) == (0x52585349, 4)
        sample_offset = 2080+5*runs
        f.seek(sample_offset); bits, width = struct.unpack('<QB', f.read(9))
        assert bits == runs*width and 1 <= width <= 64
    assert os.path.getsize(a.heads) == runs*8
    heads = np.memmap(a.heads, dtype='<u8', mode='r')
    lengths = np.memmap(a.ri4, dtype='<u4', mode='r', offset=2080+runs, shape=(runs,))
    words = np.memmap(a.ri4, dtype='<u8', mode='r', offset=sample_offset+9, shape=((bits+63)//64,))
    def tail(j):
        bit = j*width; w, shift = divmod(bit, 64)
        value = int(words[w]) >> shift
        if shift+width > 64: value |= int(words[w+1]) << (64-shift)
        value &= (1 << width)-1
        assert value < n
        return n-1-value
    with open(a.anchors, 'rb') as f:
        magic, count = struct.unpack('<IQ', f.read(12)); assert magic == 0x434E4158
        anchors = np.fromfile(f, dtype='<u8').reshape((-1, 2)); assert len(anchors) == count == k
    anchors = anchors[np.argsort(anchors[:, 0])]
    assert np.all(anchors < n)
    assert np.all(anchors[1:, 0] > anchors[:-1, 0])
    cursor = 0; head_checks = tail_checks = interior = 0
    for begin in range(0, runs, CHUNK):
        h = heads[begin:begin+CHUNK]; assert np.all(h < n)
        ends = np.cumsum(lengths[begin:begin+CHUNK], dtype=np.uint64)+cursor
        lo = np.searchsorted(anchors[:, 0], cursor)
        hi = np.searchsorted(anchors[:, 0], ends[-1])
        for row, sample in anchors[lo:hi]:
            row, sample = int(row), int(sample)
            j = int(np.searchsorted(ends, row, side='right'))
            start = cursor if j == 0 else int(ends[j-1])
            # XANC stores mirrored text coordinate S, just like ri4 samples.
            expected = n-1-sample
            hit = False
            if row == start:
                assert int(h[j]) == expected, (row, begin+j, int(h[j]), expected)
                head_checks += 1; hit = True
            if row == int(ends[j])-1:
                assert tail(begin+j) == expected, (row, begin+j, tail(begin+j), expected)
                tail_checks += 1; hit = True
            if not hit: interior += 1
        cursor = int(ends[-1])
    assert cursor == n and head_checks > 0
    print(f'PASS all {runs} heads < n={n}; anchor head_checks={head_checks} tail_checks={tail_checks} interior_unchecked={interior} anchors={count}', flush=True)
