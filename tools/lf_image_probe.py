#!/usr/bin/env python3
"""lf_image_probe.py — aggregate toehold-walk cost of resolving EVERY run's
head/tail text position via LF-images + run-end samples.

Structural fact (standard): LF maps a BWT run's rows to a CONSECUTIVE row
block [C[c]+s, C[c]+s+len) computable in O(1) from C + run lengths (.ri4
data, no walking). SA[LF(b)] = SA[b] - 1, so a sample at (or a toehold walk
from) the image's END row resolves the run's TAIL; the image's START row
resolves the HEAD. The open quantity: the walk-length distribution summed
over all runs. If sum ~ O(r): chi construction in O(r). If ~ O(r log r):
the O(r log r) conjecture gets its algorithm shape.

Pure-array version (brute SA/LCP at synth scale); the arithmetic is the
same the .ri4 would feed a Rust port.
"""
import random
import sys


def suffix_array(x):
    # dense tie-aware initial ranks (see scatter_probe.suffix_array note)
    n = len(x)
    sa = sorted(range(n), key=lambda i: x[i])
    rank = [0] * n
    for idx, p in enumerate(sa):
        if idx > 0 and x[p] == x[sa[idx - 1]]:
            rank[p] = rank[sa[idx - 1]]
        else:
            rank[p] = idx
    k = 1
    while True:
        key = lambda i: (rank[i], rank[i + k] if i + k < n else -1)
        sa.sort(key=key)
        tmp = [0] * n
        tmp[sa[0]] = 0
        for i in range(1, n):
            tmp[sa[i]] = tmp[sa[i - 1]] + (key(sa[i]) != key(sa[i - 1]))
        rank = tmp[:]
        if rank[sa[-1]] == n - 1:
            break
        k *= 2
    return sa


def probe(name, x):
    N = len(x) + 1
    X = x + b"\x00"
    sa = suffix_array(X)
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]

    # runs + per-char prefix row counts
    run_id = [0] * N
    runs = []  # (char, start, end)
    r = 1
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            runs.append((BWT[i - 1], i - 1 - (runs[-1][2] if runs else -1) + (0), i - 1))
            r += 1
        run_id[i] = r - 1
    runs.append((BWT[N - 1], N - 1 - (runs[-1][2] if runs else -1), N - 1))
    # rebuild clean run list
    runs = []
    st = 0
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            runs.append((BWT[i - 1], st, i - 1))
            st = i
    runs.append((BWT[N - 1], st, N - 1))
    for j, (c, a, b) in enumerate(runs):
        for i in range(a, b + 1):
            run_id[i] = j
    R = len(runs)

    # C table over rows: C[c] = first row with suffix starting after all
    # chars < c; and rank_c via prefix counts
    sigma = max(BWT) + 1
    cnt = [0] * sigma
    for c in BWT:
        cnt[c] += 1
    C = [0] * sigma
    acc = 0
    for c in range(sigma):
        C[c] = acc
        acc += cnt[c]
    # rank_c(k) = number of c-rows before row k: build per-char positions
    char_rows = [[] for _ in range(sigma)]
    for i in range(N):
        char_rows[BWT[i]].append(i)

    # samples: SA at run ends
    sample = [sa[b] for (c, a, b) in runs]

    # next-run-end-at-or-after row i: precompute
    next_end = [0] * N
    j = 0
    for i in range(N - 1, -1, -1):
        while runs[j][1] > i:
            j -= 1
        # find run containing i, its end
        pass
    # simpler: run_id gives enclosing run; end = runs[run_id[i]][2]
    def walk_to_sample(row):
        t = 0
        while True:
            ri = run_id[row]
            c, a, b = runs[ri]
            if row == b:
                return sample[ri], t
            # LF step
            cch = BWT[row]
            # rank_c(row): index of row among c-rows
            lo, hi = 0, len(char_rows[cch])
            # binary search position of 'row' in char_rows[cch]
            import bisect
            k = bisect.bisect_left(char_rows[cch], row)
            row = C[cch] + k
            t += 1
            if t > N:
                return None, t

    import bisect
    tot_t = 0
    tot_h = 0
    hist_t = {}
    hits = 0
    maxw = 0
    for (c, a, b) in runs:
        k = bisect.bisect_left(char_rows[c], a)   # rank_c(a) = # c-rows before a
        L = C[c] + k
        ln = b - a + 1
        # TAIL: image end row e = L + ln - 1
        e = L + ln - 1
        if run_id[e] < R and e == runs[run_id[e]][2]:
            hits += 1
            tot_t += 0
        else:
            s, t = walk_to_sample(e)
            tot_t += t
            maxw = max(maxw, t)
        # HEAD: image start row L
        if run_id[L] < R and L == runs[run_id[L]][2]:
            tot_h += 0
        else:
            s, t = walk_to_sample(L)
            tot_h += t
            maxw = max(maxw, t)
    import math
    print(f"{name:24s} n={N:7d} r={R:6d} hits={hits:5d}({100*hits/R:4.1f}%) "
          f"sum-walk={tot_t+tot_h:8d} = {R and (tot_t+tot_h)/R:6.2f}r "
          f"= {(tot_t+tot_h)/max(R*math.log2(R),1):5.2f}rlog  max-walk={maxw}")


def gen():
    rng = random.Random(20260926)
    yield "random-bin-100k", bytes(rng.choice((1, 2)) for _ in range(100000))
    yield "random-4-100k", bytes(rng.choice((1, 2, 3, 4)) for _ in range(100000))
    sat = b"\x01\x02\x03" * 33000
    tail = bytes(rng.choice((1, 2, 3, 4)) for _ in range(1000))
    yield "satellite-100k", sat + tail
    yield "repeat-ab-100k", b"\x01\x02" * 50000
    yield "random-4-500k", bytes(rng.choice((1, 2, 3, 4)) for _ in range(500000))


if __name__ == "__main__":
    print(f"{'text':24s} {'n':>7s} {'r':>6s} {'hits':>9s} "
          f"{'sum-walk':>9s}  =r      =rlog   max")
    for name, x in gen():
        probe(name, x)
    print("LF-IMAGE PROBE DONE")
