#!/usr/bin/env python3
"""Normalize a legacy successor/cyclic LCP column to predecessor/finite layout.

Reads SA by rank; never constructs an inverse. Inputs are read-only. This is
an offline validation adapter, not a product stage. A same-sized column may
still belong to a different SA: run the exact product/hash gates afterward.
"""
import argparse
import os
from pathlib import Path
import struct
import tempfile
import numpy as np

HEADER = struct.Struct('<6Q')
PAIR = np.dtype([('rank', '<u8'), ('value', '<u8')])


def normalize(sa_path, source_path, output_path, chunk=1 << 20):
    sa_path, source_path, output_path = map(Path, (sa_path, source_path, output_path))
    with sa_path.open('rb') as f:
        sa_header = HEADER.unpack(f.read(48))
    with source_path.open('rb') as f:
        magic, n, rows, escapes, _, flags = HEADER.unpack(f.read(48))
    assert magic == 5424044115 and rows == n and n > 0 and flags == 1
    assert (sa_header[0] & 0xffffffff) == 0x41535853
    assert sa_header[1:5] == (n, 1, n, n)
    assert sa_path.stat().st_size == 48 + 8 * n
    assert source_path.stat().st_size == 48 + n + 16 * escapes
    if output_path.exists():
        raise FileExistsError(output_path)
    sa = np.memmap(sa_path, mode='r', offset=48, dtype='<u8', shape=(n,))
    body = np.memmap(source_path, mode='r', offset=48, dtype='u1', shape=(n,))
    old_esc = np.memmap(source_path, mode='r', offset=48 + n, dtype=PAIR, shape=(escapes,)) if escapes else []
    temporary = output_path.with_name(output_path.name + f'.partial-{os.getpid()}')
    count, maximum = 0, 0
    try:
        with temporary.open('xb') as out:
            out.write(b'\0' * 49)  # header + predecessor LCP at rank zero
            out.truncate(48 + n)
            for lo in range(1, n, chunk):
                hi = min(n, lo + chunk)
                left, right = sa[lo - 1:hi - 1], sa[lo:hi]
                assert np.all(left < n) and np.all(right < n)
                cap = n - np.maximum(left, right)
                values = np.minimum(body[lo - 1:hi - 1], cap).astype('u1')
                out.seek(48 + lo)
                out.write(values.tobytes())
                maximum = max(maximum, int(values.max()))
            out.seek(48 + n)
            previous = -1
            for lo in range(0, escapes, chunk):
                block = old_esc[lo:min(escapes, lo + chunk)]
                ranks = block['rank']
                assert int(ranks[0]) > previous and np.all(ranks[1:] > ranks[:-1])
                assert np.all(ranks < n) and np.all(block['value'] >= 255)
                previous = int(ranks[-1])
                block = block[ranks < n - 1]  # discard cyclic wrap entry
                ranks = block['rank'] + 1
                cap = n - np.maximum(sa[ranks - 1], sa[ranks])
                values = np.minimum(block['value'], cap)
                keep = values >= 255
                result = np.empty(int(keep.sum()), dtype=PAIR)
                result['rank'], result['value'] = ranks[keep], values[keep]
                out.write(result.tobytes())
                count += len(result)
                if len(result): maximum = max(maximum, int(result['value'].max()))
            out.seek(0)
            out.write(HEADER.pack(magic, n, n, count, maximum, 1))
            out.flush()
            assert out.seek(0, os.SEEK_END) == 48 + n + count * 16
        os.link(temporary, output_path)  # publish without overwriting another file
    finally:
        temporary.unlink(missing_ok=True)
    return dict(n=n, escapes=count, max_lcp=maximum, output=str(output_path), inverse_bytes=0)


def selftest():
    cases = [b'x', b'banana', b'abab', b'a' * 600, b'a' * 300 + b'b' + b'a' * 300]
    with tempfile.TemporaryDirectory(prefix='normalize-column-') as directory:
        root = Path(directory)
        for case, text in enumerate(cases):
            n = len(text)
            sa = sorted(range(n), key=lambda p: text[p:])
            def lce(a, b, cyclic):
                k, cap = 0, n if cyclic else min(n - a, n - b)
                while k < cap and text[(a + k) % n] == text[(b + k) % n]: k += 1
                return k
            successor = [lce(sa[r], sa[(r + 1) % n], True) for r in range(n)]
            predecessor = [0] + [lce(sa[r - 1], sa[r], False) for r in range(1, n)]
            sp, cp, op = [root / f'{case}.{ext}' for ext in ('sa', 'old', 'new')]
            sp.write_bytes(HEADER.pack((1 << 32) | 0x41535853, n, 1, n, n, 10) + struct.pack(f'<{n}Q', *sa))
            esc = [(r, v) for r, v in enumerate(successor) if v >= 255]
            cp.write_bytes(HEADER.pack(5424044115, n, n, len(esc), max(successor), 1) +
                           bytes(min(v, 255) for v in successor) + b''.join(struct.pack('<QQ', *e) for e in esc))
            normalize(sp, cp, op, chunk=17)
            result = op.read_bytes()
            _, _, _, count, maximum, _ = HEADER.unpack_from(result)
            actual = list(result[48:48 + n])
            for j in range(count):
                rank, value = struct.unpack_from('<QQ', result, 48 + n + 16 * j)
                actual[rank] = value
            assert actual == predecessor and maximum == max(predecessor)
    print('NORMALIZE_COLUMN_SELFTEST_PASS cases=5 singleton=1 wrap=1 finite_cap=1 escapes=1 chunks=1 inverse_bytes=0')


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--selftest', action='store_true')
    p.add_argument('--sa')
    p.add_argument('--successor-column')
    p.add_argument('--output')
    a = p.parse_args()
    if a.selftest:
        selftest()
    else:
        assert a.sa and a.successor_column and a.output
        print('NORMALIZED_COLUMN_DONE', normalize(a.sa, a.successor_column, a.output), flush=True)
