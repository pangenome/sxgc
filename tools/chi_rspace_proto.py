#!/usr/bin/env python3
"""chi_rspace_proto.py — END-TO-END r-space chi/sA construction (assembly
of the two gated components), battery scale, oracle-gated.

PIPELINE (linear convention X = T + \\x00, N = |T|+1; machinery rows of the
PFP resolver map to linear rows j by: machinery row = j + W - 1 for j >= 1,
positions identical — verified by parse_resolve_proto's gate):

  INPUTS (r-space-shaped; brute is used ONLY to BUILD them and to verify):
    r1  runs of the linear BWT (char, start, end)   [the rlbwt]
    r2  PFP artifacts -> PFPResolve                  [parse coordinates]
    r3  piece starts + PLCP samples (direct law: a piece continues at p
        iff PLCP(p-1) = PLCP(p)+1)                  [the phi structures]
  CONSTRUCTION PHASE (no SA array, no LF-walk, no text, no brute LCP):
    step 1  run edges from r1 (r-space, no resolution needed to FIND
            them); resolve their positions (2r resolutions).
    step 2  HONEST O(n) FALLBACK for cell identification: resolve ALL
            rows to place each row's position in a piece, then take
            per-(run, piece) argmax-SA and argmin-SA rows as the cell
            extremes. The count is reported; the O(r) refinement (the
            E(r,I) derivation) is a later lane. Only the EXTREME rows
            are kept; the other resolutions are discarded.
    step 3  event rows = run-edge rows UNION cell extremes; LCP of each
            event row evaluated by the PIECE LAW (r3): PLCP(pos) =
            sample - (pos - start) — never the brute LCP array.
    step 4  restricted-PSV/NSV sweep (events_probe logic, 'both' mode)
            with the FM rule -> witness values N - resolved_position.
    step 5  dedupe -> sA set; chi = |sA|.

GATE: constructed witness SET == full-FM oracle witness set (and chi ==
|oracle|), on the shared battery (alphabet 0x0b..0x0e for pfp++).
"""
import bisect
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from scatter_probe import kasai_lcp, suffix_array, sv
from parse_resolve_proto import DOLLAR, PFPResolve, W, run_pfp

SIGMA = 256


# ---------------------------------------------------------------- oracle ---

def full_fm_oracle(T):
    """Full FM (linear convention) with FULL PSV/NSV: the emitted witness
    set (values N - SA[ip]) and its size. Pure brute; the gate's truth."""
    N = len(T) + 1
    X = T + b"\x00"
    sa, _ = suffix_array(X)
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    LCP = kasai_lcp(X, sa, isa)
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    psvL, nsvL = sv(LCP)
    R = [(-2, 0, False, 2**62)] * SIGMA
    S = []
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= psvL[i]:
                        if R[c][3] < i:
                            S.append(R[c][1])
                        R[c] = (i, N - sa[ip], True, nsvL[i])
    for c in range(1, SIGMA):
        if R[c][2]:
            S.append(R[c][1])
    return set(S), sa, isa, LCP, BWT


# ------------------------------------------------------- r-space inputs ---

def build_rspace_inputs(T, sa, isa, LCP, BWT):
    """Build the three r-space-shaped inputs. Uses brute ONLY here."""
    N = len(T) + 1
    # r1: runs of the linear BWT
    runs = []
    st = 0
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            runs.append((BWT[i - 1], st, i - 1))
            st = i
    runs.append((BWT[N - 1], st, N - 1))
    # r3: direct-law pieces over positions [0, N)
    PLCP = [LCP[isa[p]] for p in range(N)]
    starts = [0]
    samples = [PLCP[0]]
    for p in range(1, N):
        if PLCP[p - 1] != PLCP[p] + 1:
            starts.append(p)
            samples.append(PLCP[p])
    return runs, starts, samples


class Pieces:
    """r3 interface: pieces(pos) -> interval index; plcp(pos) -> value by
    the piece law. (Production: the TeraLCP phi lookup, same interface.)"""

    def __init__(self, starts, samples):
        self.starts = starts
        self.samples = samples
        self.lookups = 0

    def index(self, pos):
        self.lookups += 1
        return bisect.bisect_right(self.starts, pos) - 1

    def plcp(self, pos):
        i = self.index(pos)
        return self.samples[i] - (pos - self.starts[i])


# ------------------------------------------------------- construction -----

def construct(T, prefix, runs, pieces, verbose=True):
    """The CONSTRUCTION PHASE. Returns (sA_set, chi, stats)."""
    N = len(T) + 1
    # --- resolve interface: linear row j >= 1 -> machinery row j+W-1 ---
    R = PFPResolve(prefix)
    assert R.n == len(T) + W, (R.n, len(T) + W)
    res_ops = [0]

    def resolve_lin(j):
        if j == 0:
            return len(T)  # terminator row: position |T| (a convention fact)
        res_ops[0] += 1
        return R.resolve(j + W - 1)

    # r1 -> run_id array + boundary rows (r-space: O(r) entries; the
    # per-row array is a prototype convenience for bisect-free lookup)
    run_id = [0] * N
    for ri, (c, a, b) in enumerate(runs):
        for j in range(a, b + 1):
            run_id[j] = ri
    bnd = set()
    for (c, a, b) in runs:
        bnd.add(a)
        bnd.add(b)
    bnd.discard(0)
    bnd = {j for j in bnd if j < N}

    # --- step 2: HONEST resolve-all fallback for cell identification ---
    # positions of all rows; only per-(run,piece) extremes are kept.
    pos = [0] * N
    for j in range(N):
        pos[j] = resolve_lin(j)
    fallback_resolutions = res_ops[0]
    # piece of each position -> cells
    iv_of_pos = [pieces.index(p) for p in pos]
    mx = {}
    mn = {}
    for j in range(N):
        key = (run_id[j], iv_of_pos[pos[j]])
        if key not in mx or pos[j] > pos[mx[key]]:
            mx[key] = j
        if key not in mn or pos[j] < pos[mn[key]]:
            mn[key] = j
    extremes = set(mx.values()) | set(mn.values())
    events = sorted(bnd | extremes)

    # --- step 3: LCP of event rows via the PIECE LAW (never brute LCP) ---
    evlcp = [(j, pieces.plcp(pos[j])) for j in events]
    ev_idx = {j: i for i, j in enumerate(events)}

    # --- step 4: restricted PSV/NSV at boundary rows + FM sweep ---
    RPSV = {}
    RNSV = {}
    for i in bnd:
        ii = ev_idx[i]
        p = -1
        for q in range(ii - 1, -1, -1):
            if evlcp[q][1] < evlcp[ii][1]:
                p = evlcp[q][0]
                break
        RPSV[i] = p
        nn = N
        for q in range(ii + 1, len(events)):
            if evlcp[q][1] < evlcp[ii][1]:
                nn = evlcp[q][0]
                break
        RNSV[i] = nn
    Rcand = [(-2, 0, False, 2**62)] * SIGMA
    S = []
    bwt_at = lambda j: runs[run_id[j]][0]
    for i in range(1, N):
        if bwt_at(i) != bwt_at(i - 1):
            for ip in (i - 1, i):
                c = bwt_at(ip)
                if c != 0:
                    if Rcand[c][0] <= RPSV[i]:
                        if Rcand[c][3] < i:
                            S.append(Rcand[c][1])
                        Rcand[c] = (i, N - pos[ip], True, RNSV[i])
    for c in range(1, SIGMA):
        if Rcand[c][2]:
            S.append(Rcand[c][1])
    sa_set = set(S)
    stats = {
        "r": len(runs),
        "events": len(events),
        "boundary": len(bnd),
        "extremes": len(extremes),
        "resolve_ops_total": res_ops[0],
        "fallback_resolutions": fallback_resolutions,
        "piece_lookups": pieces.lookups,
        "sweep_rows": N,
    }
    if verbose:
        # interface self-check: piece-law LCP == brute LCP at event rows
        # (cheap, and it validates the r3 interface end-to-end)
        pass
    return sa_set, len(sa_set), stats, pos


# ------------------------------------------------------------------ gate ---

def battery():
    rng = random.Random(20261001)
    texts = []
    texts.append(("random-4-20k", bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(20000))))
    texts.append(("satellite-18k", b"\x0b\x0c\x0d" * 6000 + bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(500))))
    texts.append(("HOR-nested", (b"\x0b\x0c\x0d" * 100 + b"\x0e") * 60 + bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(300))))
    paras = [bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(300)) for _ in range(40)]
    docs = []
    for _ in range(50):
        for p in rng.sample(paras, len(paras)):
            docs.append(p)
    texts.append(("duplicates-600k", b"".join(docs)[:600000]))
    parts = []
    for i in range(400):
        parts.append(paras[rng.randrange(40)] if rng.random() < 0.5
                     else bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(300)))
    texts.append(("dup+unique-120k", b"".join(parts)))
    return texts


def main():
    os.makedirs("/tmp/crp", exist_ok=True)
    os.chdir("/tmp/crp")
    print(f"{'text':16s} {'n':>7s} {'r':>7s} {'chi-orth':>8s} {'chi-rs':>7s} "
          f"{'seteq':>6s} {'events':>7s} {'res(n)':>8s} {'pieceL':>7s}")
    allpass = True
    for name, T in battery():
        prefix = "a_" + name
        run_pfp(T, prefix)
        oracle, sa, isa, LCP, BWT = full_fm_oracle(T)
        runs, starts, samples = build_rspace_inputs(T, sa, isa, LCP, BWT)
        pieces = Pieces(starts, samples)
        N = len(T) + 1
        sa_set, chi, stats, pos = construct(T, prefix, runs, pieces)
        # interface assert: piece-law LCP == brute LCP at ALL rows (we have
        # pos anyway; this validates r3 end-to-end, cost reported separately)
        plcp_bad = 0
        for j in range(N):
            if pieces.plcp(pos[j]) != LCP[j]:
                plcp_bad += 1
        seteq = sa_set == oracle
        ok = seteq and chi == len(oracle) and plcp_bad == 0
        allpass &= ok
        print(f"{name:16s} {len(T):7d} {stats['r']:7d} {len(oracle):8d} {chi:7d} "
              f"{('YES' if seteq else 'NO'):>6s} {stats['events']:7d} "
              f"{stats['resolve_ops_total']:8d} {stats['piece_lookups']:7d} "
              f"{'plcp-bad=' + str(plcp_bad) if plcp_bad else ''}")
        if not seteq:
            only_con = sa_set - oracle
            only_orth = oracle - sa_set
            print(f"    construction-only witnesses: {len(only_con)} sample {sorted(only_con)[:5]}")
            print(f"    oracle-only witnesses:       {len(only_orth)} sample {sorted(only_orth)[:5]}")
    print("CHI-RSPACE ASSEMBLY GATE:", "ALL PASS" if allpass else "FAILURES PRESENT")
    print("ASSEMBLY DONE")


if __name__ == "__main__":
    main()
