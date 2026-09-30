#!/usr/bin/env python3
"""Small independent cyclic-SA oracle for the candidate run-tail phi map."""
import bisect
import random


def check(text):
    n = len(text)
    sa = sorted(range(n), key=lambda i: text[i:] + text[:i])
    bwt = bytes(text[(i - 1) % n] for i in sa)
    ends = [i for i in range(1, n + 1) if i == n or bwt[i] != bwt[i - 1]]
    inverse = [0] * n
    for row, value in enumerate(sa):
        inverse[value] = row
    actual = [sa[(inverse[x] + 1) % n] for x in range(n)]
    pairs = sorted((sa[e - 1], sa[e % n]) for e in ends)
    predicted = []
    for x in range(n):
        i = bisect.bisect_right(pairs, (x, n)) - 1
        u, v = pairs[i]
        if i < 0:
            u -= n
        predicted.append((v + x - u) % n)
    return [(x, a, b) for x, (a, b) in enumerate(zip(predicted, actual)) if a != b]


for text, mismatch in [(b"banana\x1e", False), (b"ACG\x1e" * 4, True),
                       (b"\x01\x02\x02" * 2, True)]:
    bad = check(text)
    assert bool(bad) == mismatch
    print(repr(text), "first_mismatch", bad[:1])

rng = random.Random(2)
aperiodic = periodic = 0
for n in range(2, 31):
    for _ in range(1000):
        text = bytes(rng.randrange(3) for _ in range(n))
        is_periodic = any(n % p == 0 and text == text[:p] * (n // p)
                          for p in range(1, n))
        bad = check(text)
        if not is_periodic:
            assert not bad, (text, bad)
            aperiodic += 1
        elif bad:
            periodic += 1
print("PASS aperiodic", aperiodic, "periodic counterexamples", periodic)
