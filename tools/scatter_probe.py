#!/usr/bin/env python3
"""scatter_probe.py — measure the (BWT-run, phi-interval) incidence structure.

The Bit-2a reduction: chi-selection = per-(run, phi-interval) extreme points
(argmax text position; piecewise-linear PLCP makes interior minima = argmax).
Whether chi is O(r)-time computable depends on the incidence structure:
  P = #{(run, interval) : nonempty}   -- the scatter
This probe measures P, its ratio to r and chi, per-run interval contiguity
(do a run's intervals form O(1) contiguous blocks?), and verifies the
reduction end-to-end (every FM witness is its pair's argmax).
Convention: fm.cpp single-string, 0-terminator, PLCP over forward text.
"""
import random
import sys
from collections import defaultdict


def suffix_array(x):
    # prefix doubling, O(n log^2 n). Initial ranks are DENSE (equal chars
    # share rank) — distinct initial ranks silently break the doubling for
    # any text with repeated characters (caught 2026-09-29 by a Lean
    # differential; verify against brute on every change).
    n = len(x)
    sa = sorted(range(n), key=lambda i: x[i])
    rank = [0] * n
    for idx, p in enumerate(sa):
        if idx > 0 and x[p] == x[sa[idx - 1]]:
            rank[p] = rank[sa[idx - 1]]
        else:
            rank[p] = idx
    k = 1
    tmp = [0] * n
    while True:
        key = lambda i: (rank[i], rank[i + k] if i + k < n else -1)
        sa.sort(key=key)
        tmp[sa[0]] = 0
        for i in range(1, n):
            tmp[sa[i]] = tmp[sa[i - 1]] + (key(sa[i]) != key(sa[i - 1]))
        rank = tmp[:]
        if rank[sa[-1]] == n - 1:
            break
        k *= 2
    return sa, rank


def kasai_lcp(x, sa, isa):
    n = len(x)
    lcp = [0] * n
    k = 0
    for p in range(n):
        if isa[p] == 0:
            k = 0
            continue
        q = sa[isa[p] - 1]
        while p + k < n and q + k < n and x[p + k] == x[q + k]:
            k += 1
        lcp[isa[p]] = k
        if k:
            k -= 1
    return lcp


def sv(LCP):
    N = len(LCP)
    nsv = [N] * N
    psv = [-1] * N
    st_n, st_p = [], []
    for i in range(N):
        while st_n and LCP[i] < LCP[st_n[-1]]:
            nsv[st_n[-1]] = i
            st_n.pop()
        st_n.append(i)
        while st_p and LCP[i] <= LCP[st_p[-1]]:
            st_p.pop()
        if st_p:
            psv[i] = st_p[-1]
        st_p.append(i)
    return psv, nsv


def fm_chi_and_selection(x):
    """x: raw text WITHOUT terminator (chars 1..sigma-1). Returns
    (chi, selected_rows) with rows in SA order, fm.cpp port."""
    N = len(x) + 1
    X = x + b"\x00"
    sa, isa = suffix_array(X)
    LCP = kasai_lcp(X, sa, isa)
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    PSV, NSV = sv(LCP)
    sigma = max(BWT) + 1
    R = [(-2, 0, False, 2**62)] * sigma   # (sa_pos, text_pos, active, nsv)
    S = []
    sel_rows = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= PSV[i]:
                        if R[c][3] < i:
                            S.append(R[c][1])
                            sel_rows.append(R[c][4] if len(R[c]) > 4 else None)
                        R[c] = (i, N - sa[ip], True, NSV[i])
    for c in range(1, sigma):
        if R[c][2]:
            S.append(R[c][1])
    return N, sa, LCP, BWT, len(set(S))


def measure(name, x):
    N = len(x) + 1
    X = x + b"\x00"
    sa, isa = suffix_array(X)
    LCP = kasai_lcp(X, sa, isa)
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    PSV, NSV = sv(LCP)
    sigma = max(BWT) + 1
    R = [(-2, 0, False, 2**62)] * sigma
    S = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= PSV[i]:
                        if R[c][3] < i:
                            S.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, NSV[i])
    for c in range(1, sigma):
        if R[c][2]:
            S.append(R[c][1])
    chi = len(set(S))

    # runs
    run_id = [0] * N
    r = 1
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            r += 1
        run_id[i] = r - 1

    # phi-intervals (production law): new piece at p iff phi(p-1) != phi(p) - 1,
    # where phi(p) = SA[ISA[p]-1] (text pos of the SA-predecessor of suffix p).
    # phi(p-1) undefined for p-1 = 0 (rank-0 suffix): break there too.
    phi = [0] * N
    for p in range(N):
        phi[p] = sa[isa[p] - 1] if isa[p] > 0 else -1
    iv = [0] * N
    cur = 0
    for p in range(1, N):
        if phi[p - 1] == -1 or phi[p] == -1 or phi[p - 1] != phi[p] - 1:
            cur += 1
        iv[p] = cur
    n_iv = cur + 1
    # slope law EXACT within pieces: PLCP(p) = PLCP(st) - (p - st)
    PLCP = [0] * N
    for p in range(N):
        PLCP[p] = LCP[isa[p]]
    law_bad = 0
    for p in range(1, N):
        st = p
        while st > 0 and iv[st - 1] == iv[p]:
            st -= 1
        if PLCP[p] != PLCP[st] - (p - st):
            law_bad += 1
    if law_bad:
        print(f"  !! slope law violated at {law_bad} positions — pieces wrong")

    # scatter
    per_run = defaultdict(set)
    argmax = {}
    for i in range(N):
        key = (run_id[i], iv[sa[i]])
        per_run[run_id[i]].add(iv[sa[i]])
        if key not in argmax or sa[i] > argmax[key]:
            argmax[key] = sa[i]
    P = sum(len(v) for v in per_run.values())

    # contiguity: number of maximal contiguous interval blocks per run
    blocks = 0
    for ivs in per_run.values():
        s = sorted(ivs)
        blocks += 1
        for a, b in zip(s, s[1:]):
            if b != a + 1:
                blocks += 1

    # verify reduction: every FM witness is its pair's argmax
    bad = 0
    for tp in set(S):
        pos = N - tp          # text position (SA coords)
        i = isa[pos]
        key = (run_id[i], iv[pos])
        if argmax[key] != pos:
            bad += 1

    print(f"{name:34s} n={N:7d} r={r:6d} iv={n_iv:6d} P={P:6d} "
          f"chi={chi:6d} P/r={P/max(r,1):5.2f} P/chi={P/max(chi,1):5.2f} "
          f"blocks/run={blocks/max(r,1):5.2f} red-viol={bad} law-bad={law_bad}")
    return r, P, chi, bad


def gen_texts():
    rng = random.Random(20260925)
    out = []
    # random binary, growing
    for n in (1000, 5000, 20000):
        out.append((f"random-bin-{n}", bytes(rng.choice((1, 2)) for _ in range(n))))
    # random 4-letter (DNA-ish)
    for n in (1000, 5000, 20000):
        out.append((f"random-4-{n}", bytes(rng.choice((1, 2, 3, 4)) for _ in range(n))))
    # periodic satellite + unique tail (mega-run stress)
    for rep in (500, 2000):
        sat = b"\x01\x02\x03" * (rep // 3 * 2)
        tail = bytes(rng.choice((1, 2, 3, 4)) for _ in range(400))
        out.append((f"satellite-{len(sat)}", sat + tail))
    # highly repetitive: (ab)^k + noise
    for k in (500, 3000):
        base = b"\x01\x02" * k
        noise = bytes(rng.choice((1, 2, 3, 4)) for _ in range(200))
        out.append((f"repeat-ab-{k}", base + noise))
    # run-length stress: a^k b a^k
    out.append(("runlength-a2k", b"\x01" * 2000 + b"\x02" + b"\x01" * 2000))
    # random 4-letter large
    out.append((f"random-4-100000", bytes(rng.choice((1, 2, 3, 4)) for _ in range(100000))))
    return out


if __name__ == "__main__":
    print(f"{'text':34s} {'n':>7s} {'r':>6s} {'iv':>6s} {'P':>6s} "
          f"{'chi':>6s} {'P/r':>5s} {'P/chi':>5s} {'blk/run':>7s} red-viol law-bad")
    for name, x in gen_texts():
        try:
            measure(name, x)
        except RecursionError:
            print(f"{name}: recursion error")
