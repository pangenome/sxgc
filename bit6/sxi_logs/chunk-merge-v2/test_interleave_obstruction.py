#!/usr/bin/env python3
"""Check whether the requested stable run interleave can equal cyclic BWT(A+B).

This is an exhaustive merge feasibility check over individual BWT characters.
If no character-level stable interleave exists, no run-level stable interleave
can exist either. It uses no corpus and does not implement the requested merge.
"""
from functools import lru_cache
import random


def cyclic_bwt(text):
    sa = sorted(range(len(text)), key=lambda i: (text[i:] + text[:i], i))
    return bytes(text[(i - 1) % len(text)] for i in sa), sa


def can_stably_interleave(a, b, target):
    @lru_cache(None)
    def reachable(i, j):
        if i + j == len(target):
            return True
        return ((i < len(a) and a[i] == target[i + j] and reachable(i + 1, j))
                or (j < len(b) and b[j] == target[i + j] and reachable(i, j + 1)))

    return reachable(0, 0)


def test_document_aligned_counterexample():
    # 0x06 and 0x07 are legal post-remap parser symbols; 0x1e is fixed.
    a = bytes((6, 6, 0x1e, 6, 0x1e))
    b = bytes((7, 6, 6, 0x1e))
    assert a[-1] == b[-1] == 0x1e
    ba, sa = cyclic_bwt(a)
    bb, sb = cyclic_bwt(b)
    merged, sm = cyclic_bwt(a + b)
    assert sa == [0, 3, 1, 4, 2]
    assert sb == [1, 2, 0, 3]
    assert sm == [6, 0, 7, 1, 3, 5, 8, 2, 4]
    assert ba == bytes.fromhex('1e 1e 06 06 06')
    assert bb == bytes.fromhex('07 06 1e 06')
    assert merged == bytes.fromhex('07 1e 06 06 1e 1e 06 06 06')
    assert not can_stably_interleave(ba, bb, merged)


def test_twenty_synthetic_aligned_pairs():
    rng = random.Random(0xC4A6)
    impossible = 0
    for _ in range(20):
        a = b''.join(bytes(rng.randrange(6, 9) for _ in range(rng.randrange(1, 5)))
                     + b'\x1e' for _ in range(2))
        b = b''.join(bytes(rng.randrange(6, 9) for _ in range(rng.randrange(1, 5)))
                     + b'\x1e' for _ in range(2))
        ba, _ = cyclic_bwt(a)
        bb, _ = cyclic_bwt(b)
        whole, _ = cyclic_bwt(a + b)
        impossible += not can_stably_interleave(ba, bb, whole)
    assert impossible > 0
    return impossible


if __name__ == '__main__':
    test_document_aligned_counterexample()
    impossible = test_twenty_synthetic_aligned_pairs()
    print(f'PASS: exact cyclic BWT cannot be stably interleaved on the fixed fixture and {impossible}/20 synthetic aligned pairs')
