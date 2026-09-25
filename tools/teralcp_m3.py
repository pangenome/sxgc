#!/usr/bin/env python3
"""teralcp_m3.py — M3: build the DIRECT-LAW pieces in parse space.

Pipeline (no brute PLCP, no text scan):
  1. candidates C = { resolve(row i) : i boundary row } U { N-1 }
     (M2: this set contains 100% of piece starts; |C| = r)
  2. for each p in C compute PLCP(p), PLCP(p-1) via a PARSE-SPACE LCP
     (phrase-occurrence character access + phrase walking; the suffix
     pair (p, phi(p)) with phi(p) = resolve(resolve_inv(p)-1))
  3. S_emit = {0} U { p in C : p>=1 and PLCP(p) != PLCP(p-1) - 1 }
     samples = PLCP at each emitted start
GATE (vs brute):
  * S_emit == brute direct-law piece starts (exact set)
  * samples match brute PLCP at those starts (to the value)
  * slope law holds on 1000 random positions per text.
"""
import bisect
import os
import sys
import random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from c_probe_common import build, suffix_array_np  # gated brute (gate only)
from parse_resolve_proto import PFPResolve, W, ENDOFWORD, run_pfp  # gated parse machinery
from c_probe_r3 import InvResolve  # gated resolve_inv
from endpoint_common import battery


class ParseText:
    """Text access + parse-space LCP, from .dict/.parse only."""

    def __init__(self, prefix):
        self.prefix = prefix
        self.R = PFPResolve(prefix)
        d = open(prefix + ".dict", "rb").read()
        phrases = []
        i = 0
        while i < len(d) - 1:
            j = i
            while j < len(d) - 1 and d[j] != ENDOFWORD:
                j += 1
            phrases.append(bytes(d[i:j]))
            i = j + 1
        self.phrases = phrases
        self.parse = self.R.parse
        self.starts = self.R.starts
        n_occ = len(self.starts)
        self.spans = [(self.starts[k + 1] - self.starts[k]) if k + 1 < n_occ
                      else (self.R.n - self.starts[k]) for k in range(n_occ)]
        # segment origin within each phrase = len(phrase) - span
        self.orig = []
        for k in range(n_occ):
            ph = phrases[self.parse[k] - 1]
            self.orig.append(len(ph) - self.spans[k])
        self.n = self.R.n
        self.lcp_steps = 0
        self.lcp_calls = 0

    def char(self, p):
        k = bisect.bisect_right(self.starts, p) - 1
        return self.phrases[self.parse[k] - 1][self.orig[k] + (p - self.starts[k])]

    def lcp(self, p, q, cap):
        """lcp of suffixes at text positions p, q (p,q < |T|), capped."""
        if p == q:
            return cap
        kp = bisect.bisect_right(self.starts, p) - 1
        kq = bisect.bisect_right(self.starts, q) - 1
        op = p - self.starts[kp]
        oq = q - self.starts[kq]
        self.lcp_calls += 1
        l = 0
        while l < cap:
            # phrase-skip: both at segment origin, same phrase id, aligned
            if op == 0 and oq == 0 and self.parse[kp] == self.parse[kq]:
                sp = self.spans[kp]
                if l + sp > cap:
                    break
                l += sp
                kp += 1
                kq += 1
                if kp >= len(self.starts) or kq >= len(self.starts):
                    break
                op = oq = 0
                continue
            ph_p = self.phrases[self.parse[kp] - 1]
            ph_q = self.phrases[self.parse[kq] - 1]
            c1 = ph_p[self.orig[kp] + op]
            c2 = ph_q[self.orig[kq] + oq]
            self.lcp_steps += 1
            if c1 != c2:
                return l
            l += 1
            op += 1
            oq += 1
            if op == self.spans[kp]:
                kp += 1
                if kp >= len(self.starts):
                    return l
                op = 0
            if oq == self.spans[kq]:
                kq += 1
                if kq >= len(self.starts):
                    return l
                oq = 0
        return l


def run(name, T, workdir="/tmp/tlc_m3"):
    os.makedirs(workdir, exist_ok=True)
    prefix = os.path.join(workdir, name)
    run_pfp(T, prefix)
    PT = ParseText(prefix)
    R = PT.R
    IR = InvResolve(prefix)
    N = len(T) + 1

    def resolve_row(j):
        """linear row j -> text position."""
        if j == 0:
            return len(T)
        return R.resolve(j + W - 1)

    # --- candidates: boundary rows resolved + N-1 ---
    S = build(T)  # brute (gate/reference only)
    BWT, sa = S["BWT"], S["sa"]
    C = set()
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            C.add(resolve_row(i))
    C.add(N - 1)
    C = sorted(C)

    # --- parse-space PLCP ---
    def phi_pos(p):
        # resolve_inv returns a MACHINERY row; R.resolve takes a machinery
        # row and returns the text position directly.  Machinery rows
        # 0..W-1 are the W leading '$' rows (positions |T|..|T|+W-1).
        row, err = IR.resolve_inv(p)
        if row is None or row == 0:
            return None
        if row - 1 < W:
            return len(T) + (row - 1)
        return R.resolve(row - 1)

    def plcp(p):
        if p >= len(T):
            return 0
        q = phi_pos(p)
        if q is None or q >= len(T):
            return 0
        cap = min(len(T) - p, len(T) - q)
        return PT.lcp(p, q, cap)

    # --- direct validation of the parse-space LCP vs brute PLCP ---
    rngv = random.Random(3)
    lcp_bad = 0
    lcp_n = 0
    for _ in range(400):
        p = rngv.randrange(0, len(T))
        b = S["LCP"][S["isa"][p]]
        a = plcp(p)
        lcp_n += 1
        if a != b:
            lcp_bad += 1
            if lcp_bad <= 3:
                print(f"    LCP mismatch p={p}: parse={a} brute={b}")

    S_emit = [0]
    samples = [plcp(0)]
    for p in C:
        if p == 0 or p >= N:
            continue
        a = plcp(p)
        b = plcp(p - 1)
        if a != b - 1:
            S_emit.append(p)
            samples.append(a)

    # --- gates ---
    Slaw = sorted(S["piece_start"])
    br_samples = []
    for st in Slaw:
        row = S["isa"][st]
        br_samples.append(S["LCP"][row])
    g_set = (S_emit == Slaw)
    # sample values at emitted starts vs brute
    mism = 0
    for st, sv in zip(S_emit, samples):
        brow = S["isa"][st]
        if S["LCP"][brow] != sv:
            mism += 1
    # slope law spot checks
    rng = random.Random(9)
    bad = 0
    idx = {st: i for i, st in enumerate(S_emit)}
    starts_arr = S_emit
    for _ in range(1000):
        p = rng.randrange(0, len(T))
        i = bisect.bisect_right(starts_arr, p) - 1
        pred = samples[i] - (p - starts_arr[i])
        row = S["isa"][p]
        if S["LCP"][row] != pred:
            bad += 1
    print(f"[{name:18s}] N={N:8d} r={len(S['runs']):7d} |C|={len(C):7d} |S_law|={len(Slaw):7d} "
          f"|S_emit|={len(S_emit):7d} set_eq={g_set} sample_mismatch={mism} slope_bad={bad} "
          f"lcp_bad={lcp_bad}/{lcp_n} lcp_calls={PT.lcp_calls} lcp_steps={PT.lcp_steps}", flush=True)
    return dict(name=name, N=N, r=len(S["runs"]), C=len(C), nlaw=len(Slaw),
                nemit=len(S_emit), seteq=g_set, mism=mism, slope=bad, lcpbad=lcp_bad)


def main():
    only = sys.argv[1:]
    res = []
    for name, T in battery():
        if only and name not in only:
            continue
        res.append(run(name, T))
    print("\n=== M3 SUMMARY ===")
    for x in res:
        print(f"{x['name']:18s} |C|={x['C']:7d} |S_law|={x['nlaw']:7d} |S_emit|={x['nemit']:7d} "
              f"set_eq={x['seteq']} sample_mismatch={x['mism']} slope_bad={x['slope']} lcp_bad={x['lcpbad']}")
    print("M3 DONE")


if __name__ == "__main__":
    main()
