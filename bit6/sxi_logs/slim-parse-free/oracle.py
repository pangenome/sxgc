#!/usr/bin/env python3
"""Artifact-anchored brute-force oracle for the parse-free slim.

The .agg is a pure function of (fresh.ri4 runs, fresh.head_sa samples) plus
the collection-LCE semantics over the source text. This oracle reads the
artifacts, independently validates them against the text (cyclic-BWT head
contract for EVERY run; head samples must be a permutation of [0,n); ri4
mirrored tail samples must agree with the head column on singleton runs),
then computes all four CRA1 columns by brute force from the text and
compares byte-exactly. Adapter artifact GENERATION is separately gated by
byte-identity against the legacy parse-based adapter (see the parity gate
in g0_synthetic_gate.py).
"""
import bisect
import struct

INF = (1 << 64) - 1


def read_ri4(path):
    data = open(path, 'rb').read()
    magic, ver = struct.unpack_from('<II', data, 0)
    assert magic == 0x52585349 and ver == 4, (hex(magic), ver)
    n, k, R = struct.unpack_from('<QQQ', data, 8)
    off = 32
    off += 8 * 256
    a = data[off:off + R]; off += R
    l = list(struct.unpack_from(f'<{R}I', data, off)); off += 4 * R
    saBits, saW = struct.unpack_from('<QB', data, off)
    words = data[off + 9:off + 9 + (saBits + 63) // 64 * 8]
    def sample(i):
        b0 = i * saW
        w = b0 >> 6
        # LSB-first packed read, saW <= 64
        v = struct.unpack_from('<Q', words, w * 8)[0] >> (b0 & 63)
        if (b0 & 63) + saW > 64:
            v |= struct.unpack_from('<Q', words, w * 8 + 8)[0] << (64 - (b0 & 63))
        return v & ((1 << saW) - 1)
    return n, k, R, a, l, sample


def oracle_columns(text, n, k, R, a, l, sample, heads):
    M2 = text + text
    # --- independent artifact validation against the text ---
    rows = sum(l)
    assert rows == n, f'run sum {rows} != n {n}'
    seen = set(heads)
    assert len(seen) == R and max(heads) < n, 'head samples are not a permutation'
    starts = []
    acc = 0
    for length in l:
        starts.append(acc); acc += length
    for r in range(R):
        assert text[(heads[r] - 1) % n] == a[r], \
            f'run {r}: cyclic-BWT head contract violated'
    tails = [n - 1 - sample(r) if sample(r) < n else None for r in range(R)]
    for r in range(R):
        if l[r] == 1:
            assert tails[r] == heads[r], f'singleton run {r} head/tail disagree'
    # --- collection LCE with the slim's exact semantics ---
    ends = [p for p, c in enumerate(text) if c == 0x0a]
    has_nl = 0x0a in a
    has_rs = 0x1e in a
    cyclic = has_rs or not has_nl
    if cyclic:
        assert k == 1, f'cyclic corpus must have k=1 (got {k})'
    else:
        assert k == len(ends), f'k {k} != newline count {len(ends)}'
    def coll(i, j):
        if cyclic:
            if i == j:
                return n
            ans = 0
            while ans < n:
                cap = min(n - ans, n - i, n - j)
                got = 0
                while got < cap and M2[i + got] == M2[j + got]:
                    got += 1
                ans += got
                if got < cap:
                    break
                i = (i + got) % n
                j = (j + got) % n
            return ans
        def rem(pos):
            e = bisect.bisect_left(ends, pos)
            assert e < len(ends), 'missing terminator'
            return ends[e] - pos
        cap = min(rem(i), rem(j))
        if cap == 0:
            return 0
        got = 0
        while got < cap and M2[i + got] == M2[j + got]:
            got += 1
        return min(got, cap)
    top, first_col, tail_col, interior = [], [], [], []
    prev_tail = None
    for r in range(R):
        head = heads[r]
        tail = tails[r]
        assert tail is not None and tail < n, f'run {r}: missing tail sample'
        first_col.append(head)
        tail_col.append(tail)
        top.append(0 if r == 0 else coll(prev_tail, head))
        interior.append(INF if l[r] == 1 else coll(head, tail))
        prev_tail = tail
    return top + first_col + tail_col + interior


def compare(agg_path, ri4_path, head_path, text_path):
    text = open(text_path, 'rb').read()
    n, k, R, a, l, sample = read_ri4(ri4_path)
    heads = list(struct.unpack(f'<{R}Q', open(head_path, 'rb').read()))
    expected = oracle_columns(text, n, k, R, a, l, sample, heads)
    raw = open(agg_path, 'rb').read()
    magic, R2 = struct.unpack_from('<IQ', raw, 0)
    assert magic == 0x31415243 and R2 == R, 'agg magic/R mismatch'
    got = list(struct.unpack_from(f'<{4 * R}Q', raw, 12))
    if got != expected:
        bad = next(i for i, (x, y) in enumerate(zip(got, expected)) if x != y)
        col = ['topLCP', 'saFirst', 'saLast', 'interiorMin'][bad // R]
        raise AssertionError(f'agg mismatch: first bad {col}[{bad % R}]: '
                            f'{got[bad]} != {expected[bad]}')
    return n, R, k
