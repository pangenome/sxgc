#!/usr/bin/env python3
"""Cyclic toy exploration: pin the fork-matrix semantics.

Builds the cyclic reversed-text BWT exactly as the artifact does
(R = reverse(S), cyclic suffixes, BWT[i] = R[sa_i - 1 mod n]), the runs,
the cyclic one-pass scan witnesses (2-pass cyclic adaptation of scanAux),
brute-force requirements/covSet, and examines for each witness:
  - the witnessing run + side (via head/tail SA candidates x=(n-sa)%n)
  - the covered requirements covSet(x) (brute force from text)
  - the LCP interval structure around the witnessing row
"""
import sys
from collections import defaultdict

SEP = 0x1e


def build(records):
    S = bytes()
    for r in records:
        S += r + bytes([SEP])
    n = len(S)
    R = S[::-1]
    # cyclic suffix order of R
    suff = sorted(range(n), key=lambda i: R[i:] + R[:i])
    sa = suff  # sa[row] = start in R
    bwt = [R[(i - 1) % n] for i in sa]
    # cyclic lcp between adjacent rows (lcp[i] between rows i-1 and i)
    def cyc(i):
        return R[i % n]
    lcp = [0] * n
    for i in range(n):
        a, b = sa[i - 1], sa[i]
        k = 0
        while cyc(a + k) == cyc(b + k):
            k += 1
        lcp[i] = k
    # runs
    runs = []  # (char, start_row, length)
    s = 0
    while s < n:
        e = s
        while e + 1 < n and bwt[e + 1] == bwt[s]:
            e += 1
        runs.append((bwt[s], s, e - s + 1))
        s = e + 1
    return S, R, n, sa, bwt, lcp, runs


def cyclic_scan(n, sa, bwt, lcp, passes=4):
    """scanAux over the cyclic triple stream, multiple passes; collect all
    emissions from the final pass (state fully warmed)."""
    SIGMA = 256
    out = set()
    # state: per char: (len, pos, active)
    R = {c: [-1, 0, False] for c in range(1, SIGMA)}
    prev_c, prev_sa, m = bwt[0], sa[0], 1 << 62
    for p in range(passes):
        out = set()
        for i in range(n):
            t_c, t_sa, t_lcp = bwt[i], sa[i], lcp[i]
            m2 = min(m, t_lcp)
            if t_c != prev_c:
                # evalStep at boundary
                for c in range(1, SIGMA):
                    cand = R[c]
                    if m2 < cand[0]:
                        if cand[2]:
                            out.add(cand[1])
                        R[c] = [m2, 0, False]
                # upd for prev char (row i-1 side) and new char (row i side)
                if t_lcp > R[prev_c][0] if prev_c else False:
                    pass
                def upd(c, l, pos):
                    if l > R[c][0]:
                        R[c] = [l, pos, True]
                if prev_c != 0:
                    upd(prev_c, t_lcp, (n - sa[(i - 1) % n]) % n)
                if t_c != 0:
                    upd(t_c, t_lcp, (n - t_sa) % n)
                m = 1 << 62
            else:
                m = m2
            prev_c, prev_sa = t_c, t_sa
        # final eval
        for c in range(1, SIGMA):
            cand = R[c]
            if -1 < cand[0] and cand[2]:
                out.add(cand[1])
    return out


def requirements_and_covsets(S, n):
    """Brute force: requirements (w,c) with w right-maximal, wc occurs; and
    covSet(x) for every 0-based end position x (w c ends at x, c=S[x])."""
    reqs = set()
    # all substrings w with occurrences; right-maximal: >=2 distinct next chars
    occ = defaultdict(set)  # w -> set of end positions (inclusive) of w
    for L in range(0, n):
        for i in range(n - L):
            w = S[i:i + L + 1]
            occ[w].add(i + L)  # 0-based inclusive end
    rightext = {}
    for w, ends in occ.items():
        nxt = {S[(e + 1) % n] for e in ends}
        # right-maximal in cyclic sense: w occurs and has >=2 next chars,
        # or w is the whole text (isSuffix branch is linear-only; cyclic:
        # whole-text w = only string, still right-maximal by Def (empty ok))
        rightext[w] = nxt
        # rightMaximal: cyclic text: occurs && (>=2 exts || w == whole?) --
        # Def 1: isSuffix w T || rightExts>=2; for cyclic we use >=2 exts,
        # plus w empty always.
    for w, nxt in rightext.items():
        if w == b"" or len(nxt) >= 2:
            for c in nxt:
                reqs.add((w, bytes([c])))
    covsets = defaultdict(set)
    for x in range(n):
        for (w, c) in reqs:
            wc = w + c
            L = len(wc)
            # wc ends at 0-based x: occupies [x-L+1..x] mod n
            if S[(x - L + 1) % n:(x - L + 1) % n + L] == wc or \
               (S[(x - L + 1) % n:] + S[:(x + 1) % n])[-L:] == wc:
                covsets[x].add((w, c))
    return reqs, covsets


def main():
    records = [b"AACGAAAT", b"AGGAGCCT", b"AATCACCT"]
    S, R, n, sa, bwt, lcp, runs = build(records)
    print("n =", n, "S =", S)
    print("runs:", [(chr(ch) if ch != SEP else '␞', st, ln) for ch, st, ln in runs])
    wit = cyclic_scan(n, sa, bwt, lcp)
    print("scan witnesses (0-based end pos of context; c=S[x]):", sorted(wit))
    reqs, covsets = requirements_and_covsets(S, n)
    # canonical chi via max classes
    max_classes = set()
    for x in range(n):
        cs = frozenset(covsets[x])
        if not cs:
            continue
        dominated = any(frozenset(covsets[y]) > cs for y in range(n) if covsets[y])
        if not dominated:
            max_classes.add(cs)
    print("brute chi (max coverage classes):", len(max_classes), " scan:", len(wit))
    # witness <-> run mapping via head/tail candidates
    run_of_head = {}
    run_of_tail = {}
    for r, (ch, st, ln) in enumerate(runs):
        h_sa = sa[st]
        t_sa = sa[st + ln - 1]
        x_head = (n - h_sa) % n
        x_tail = (n - t_sa) % n
        run_of_head[x_head] = r
        run_of_tail[x_tail] = r
    print("\nper-witness structure:")
    for x in sorted(wit):
        side = "HEAD" if x in run_of_head else ("TAIL" if x in run_of_tail else "NONE")
        r = run_of_head.get(x, run_of_tail.get(x))
        ch = runs[r][0] if r is not None else None
        print(f"  wit x={x:3d} run={r} side={side} runchar={chr(ch) if ch and ch!=SEP else ch} S[x..x-3]={S[x% n]}{S[(x-1)%n]}{S[(x-2)%n]} covsize={len(covsets[x])}")
        for (w, c) in sorted(covsets[x], key=lambda p: (len(p[0]), p)):
            print(f"       req w={w} c={c}  (depth {len(w)})")
    # suffixient check
    allreq = reqs
    covered = set()
    for x in wit:
        covered |= covsets[x]
    print("\nscan set suffixient:", allreq <= covered, " missing:", allreq - covered)


if __name__ == "__main__":
    main()
