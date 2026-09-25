#!/usr/bin/env python3
"""endpoint_sweep.py — TASKS 2+3: the construction with EXACT range-min
semantics via the measured endpoint law.

ARCHITECTURE (no whitelist, no O(n) LCP slices):
  * per class block b = [lo, hi): the block's LCP-slice minimum is
    MEASURED (0 violations / 40K+ non-trivial blocks, all regimes) to be
    min(LCP[lo], LCP[hi-1]) — two r-space values (resolve + piece law).
    Block-min feeds a segment tree (O(|M|) space) for skip descent.
  * PSV(i, tau): scan the own-block part [lo(i), i) right-to-left
    (probes = resolve+piece-law each, counted); if nothing sub-tau,
    segment-tree descent to the rightmost earlier block with min < tau,
    then scan that block right-to-left for its rightmost sub-tau row.
  * NSV symmetric.  Sentinel values match sv(): -1 / N.
  * FM sweep with these EXACT PSV/NSV -> witness values N - pos(ip).

GATE: witness SET == full-FM oracle on all 7 battery texts;
duplicates-600k must be 13004/13004, zero missing, zero spurious.

HONEST COSTS (counted, reported): resolutions = 2|M| (endpoint build,
the C-term residue: |M| = Theta(n) on singleton-class regimes, ~11r on
duplicates, ~1.3r on random) + probes + witness positions + tree ops.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from endpoint_common import TextCase, battery

INF = 1 << 62


class SegTree:
    def __init__(self, vals):
        self.n = len(vals)
        self.t = [INF] * (2 * self.n)
        for i, v in enumerate(vals):
            self.t[self.n + i] = v
        for i in range(self.n - 1, 0, -1):
            self.t[i] = min(self.t[2 * i], self.t[2 * i + 1])
        self.visits = 0

    def query(self, l, r):          # min over blocks [l, r)
        self.visits += 1
        res = INF
        l += self.n
        r += self.n
        while l < r:
            if l & 1:
                res = min(res, self.t[l])
                l += 1
            if r & 1:
                r -= 1
                res = min(res, self.t[r])
            l >>= 1
            r >>= 1
        return res

    def rmost_below(self, l, r, tau):  # rightmost block index in [l, r) with min < tau
        self.visits += 1
        if self.query(l, r) >= tau:
            return -1
        lo, hi = l, r
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.query(mid, hi) < tau:
                lo = mid
            else:
                hi = mid
        return lo


class EndpointSweep:
    def __init__(self, tc):
        self.tc = tc
        self.ops = {"res": 0, "probe": 0, "tree": 0}
        # endpoint values per block (r-space: resolve + piece law)
        ev = []
        for (lo, hi) in tc.blocks:
            a = self._lcp(lo)
            b = self._lcp(hi - 1)
            ev.append(min(a, b))
            self.ops["res"] += 2
        self.tree = SegTree(ev)
        # convenience: block_of array (prototype; production = bisect)
        self.block_of = tc.block_of

    def _lcp(self, j):
        self.ops["probe"] += 1
        return self.tc.plcp_of_row(j)

    def _psv(self, i, tau):
        if tau == 0:
            return -1
        b = self.block_of[i]
        lo, hi = self.tc.blocks[b]
        # own-block part [lo, i): rightmost sub-tau row
        for j in range(i - 1, lo - 1, -1):
            if self._lcp(j) < tau:
                return j
        if b == 0:
            return -1
        bb = self.tree.rmost_below(0, b, tau)
        self.ops["tree"] += 1
        if bb < 0:
            return -1
        blo, bhi = self.tc.blocks[bb]
        for j in range(bhi - 1, blo - 1, -1):
            if self._lcp(j) < tau:
                return j
        return -1   # cannot happen if block-min < tau... unless endpoint law fails

    def _nsv(self, i, tau):
        N = self.tc.N
        if tau == 0:
            return N
        b = self.block_of[i]
        lo, hi = self.tc.blocks[b]
        for j in range(i + 1, hi):
            if self._lcp(j) < tau:
                return j
        if b + 1 >= len(self.tc.blocks):
            return N
        bb = self.tree.rmost_below(b + 1, len(self.tc.blocks), tau, ) if False else None
        # leftmost block with min < tau in [b+1, end): mirror of rmost
        l, r = b + 1, len(self.tc.blocks)
        if self.tree.query(l, r) >= tau:
            return N
        self.ops["tree"] += 1
        while r - l > 1:
            mid = (l + r) // 2
            if self.tree.query(l, mid) < tau:
                r = mid
            else:
                l = mid
        blo, bhi = self.tc.blocks[l]
        for j in range(blo, bhi):
            if self._lcp(j) < tau:
                return j
        return N

    def witness_set(self):
        tc = self.tc
        N = tc.N
        bwt_at = lambda j: tc.runs[tc.run_id[j]][0]
        SIG = 256
        R = [(-2, 0, False, INF)] * SIG
        S = []
        nb = 0
        for i in range(1, N):
            if bwt_at(i) != bwt_at(i - 1):
                nb += 1
                tau = self._lcp(i)
                p = self._psv(i, tau)
                q = self._nsv(i, tau)
                for ip in (i - 1, i):
                    c = bwt_at(ip)
                    if c != 0:
                        if R[c][0] <= p:
                            if R[c][3] < i:
                                S.append(R[c][1])
                            self.ops["res"] += 1
                            pos = tc.resolve_lin(ip)
                            R[c] = (i, N - pos, True, q)
        for c in range(1, SIG):
            if R[c][2]:
                S.append(R[c][1])
        return set(S), nb


def run(name, T):
    tc = TextCase(name, T, workdir="/tmp/endpoint")
    es = EndpointSweep(tc)
    sa_set, nb = es.witness_set()
    # oracle: full FM with full sv()
    from scatter_probe import sv
    N = tc.N
    LCP, BWT, sa = tc.LCP, tc.BWT, tc.sa
    psvL, nsvL = sv(LCP)
    SIG = 256
    R = [(-2, 0, False, INF)] * SIG
    O = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= psvL[i]:
                        if R[c][3] < i:
                            O.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, nsvL[i])
    for c in range(1, SIG):
        if R[c][2]:
            O.append(R[c][1])
    oracle = set(O)
    eq = sa_set == oracle
    r = len(tc.runs)
    n = N
    M = len(tc.R.M)
    print(f"{name:16s} n={n:7d} r={r:6d} |M|={M:6d} chi={len(oracle):6d} "
          f"built={len(sa_set):6d} seteq={'YES' if eq else 'NO'}")
    if not eq:
        print(f"    missing: {sorted(oracle - sa_set)[:5]}  spurious: {sorted(sa_set - oracle)[:5]}")
    print(f"    ops: endpoint-res 2|M|={2*M}  probes={es.ops['probe']}  tree={es.ops['tree'] + es.tree.visits} "
          f"witness-res={es.ops['res'] - 2*M}   (r={r}, n={n})")
    return eq, len(oracle), len(sa_set)


if __name__ == "__main__":
    only = sys.argv[1:] or None
    allok = True
    results = []
    for name, T in battery():
        if only and name not in only:
            continue
        eq, co, cb = run(name, T)
        allok &= eq
        results.append((name, eq, co, cb))
    print("ENDPOINT SWEEP GATE:", "ALL PASS" if allok else "FAILURES PRESENT")
    for (nm, eq, co, cb) in results:
        if not eq:
            print(f"  FAIL {nm}: oracle={co} built={cb}")
    print("SWEEP DONE")
