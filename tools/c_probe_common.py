#!/usr/bin/env python3
"""c_probe_common.py — shared gated baseline for the C-term attack lane.

Contents:
  * suffix_array_np : numpy prefix-doubling SA (validated against the
    gated pure-Python suffix_array in scatter_probe on small texts).
  * build(x)        : full brute structures — SA, LCP, PLCP, BWT, runs,
    DIRECT-LAW pieces (MODEL v3: piece continues at p iff
    PLCP[p-1] == PLCP[p] + 1), cells (run, piece), both per-cell extremes.
  * chi_full(x)     : FM chi over the full PSV/NSV streams (ground truth).
  * chi_events_v3(x): FM chi with PSV/NSV restricted to the v3 event set
    (boundary rows UNION both per-cell extremes).  Must equal chi_full.
The 124/124 gate (fmTexts battery) is run by c_probe_gate.py.
"""
import random

import numpy as np

from scatter_probe import kasai_lcp, suffix_array, sv  # gated references


def suffix_array_np(x):
    """SA of bytes/int-array x via numpy prefix doubling. Returns list of ints."""
    n = len(x)
    if n == 0:
        return []
    arr = np.frombuffer(bytes(x), dtype=np.uint8).astype(np.int64) if not isinstance(x, np.ndarray) else x.astype(np.int64)
    # dense ranks of characters
    _, rank = np.unique(arr, return_inverse=True)
    rank = rank.astype(np.int64)
    k = 1
    while True:
        second = np.full(n, -1, dtype=np.int64)
        if k < n:
            second[: n - k] = rank[k:]
        key = rank * (n + 1) + (second + 1)
        order = np.argsort(key, kind="stable")
        sk = key[order]
        newrank = np.empty(n, dtype=np.int64)
        diff = np.empty(n, dtype=bool)
        diff[0] = False
        if n > 1:
            diff[1:] = sk[1:] != sk[:-1]
        newrank[order] = np.cumsum(diff)
        rank = newrank
        if int(rank[order[-1]]) == n - 1:
            return [int(v) for v in order]
        k *= 2


def build(x):
    """Brute structures under the MODEL v3 single-string convention
    (X = x + b'\\x00'; rows 0..N-1)."""
    N = len(x) + 1
    X = x + b"\x00"
    sa = suffix_array_np(X)
    assert len(sa) == N
    isa = [0] * N
    for i, p in enumerate(sa):
        isa[p] = i
    LCP = kasai_lcp(X, sa, isa)
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    # runs
    run_id = [0] * N
    runs = []  # (start_row, end_row)
    r = 1
    st = 0
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            runs.append((st, i - 1))
            st = i
            r += 1
        run_id[i] = r - 1
    runs.append((st, N - 1))
    # PLCP and DIRECT-LAW pieces
    PLCP = [LCP[isa[p]] for p in range(N)]
    piece_id = [0] * N
    piece_start = [0]
    cur = 0
    for p in range(1, N):
        if PLCP[p - 1] != PLCP[p] + 1:
            cur += 1
            piece_start.append(p)
        piece_id[p] = cur
    n_pieces = cur + 1
    # piece ends
    piece_end = [0] * n_pieces
    for p in range(N):
        piece_end[piece_id[p]] = p
    # cells + both extremes
    cell_max_row = {}
    cell_min_row = {}
    cell_max_pos = {}
    cell_min_pos = {}
    for k in range(N):
        key = (run_id[k], piece_id[sa[k]])
        pos = sa[k]
        if key not in cell_max_pos or pos > cell_max_pos[key]:
            cell_max_pos[key] = pos
            cell_max_row[key] = k
        if key not in cell_min_pos or pos < cell_min_pos[key]:
            cell_min_pos[key] = pos
            cell_min_row[key] = k
    return dict(N=N, X=X, sa=sa, isa=isa, LCP=LCP, BWT=BWT, run_id=run_id, runs=runs,
                PLCP=PLCP, piece_id=piece_id, piece_start=piece_start, piece_end=piece_end,
                n_pieces=n_pieces, cell_max_row=cell_max_row, cell_min_row=cell_min_row,
                cell_max_pos=cell_max_pos, cell_min_pos=cell_min_pos)


def _sigma(BWT):
    return max(BWT) + 1


def chi_full(S):
    """Full FM chi (ground truth)."""
    N, LCP, BWT, sa = S["N"], S["LCP"], S["BWT"], S["sa"]
    psvL, nsvL = sv(LCP)
    sigma = _sigma(BWT)
    R = [(-2, 0, False, 2**62)] * sigma
    out = set()
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= psvL[i]:
                        if R[c][3] < i:
                            out.add(R[c][1])
                        R[c] = (i, N - sa[ip], True, nsvL[i])
    for c in range(1, sigma):
        if R[c][2]:
            out.add(R[c][1])
    return len(out)


def chi_events_v3(S):
    """FM chi with PSV/NSV restricted to the v3 event set."""
    N, LCP, BWT, sa = S["N"], S["LCP"], S["BWT"], S["sa"]
    bnd = set(i for i in range(1, N) if BWT[i] != BWT[i - 1])
    ext = set(S["cell_max_row"].values()) | set(S["cell_min_row"].values())
    events = sorted(bnd | ext)
    evlcp = [(k, LCP[k]) for k in events]
    ev_idx = {k: j for j, k in enumerate(events)}
    RPSV, RNSV = {}, {}
    for i in bnd:
        j = ev_idx[i]
        p = -1
        for q in range(j - 1, -1, -1):
            if evlcp[q][1] < LCP[i]:
                p = evlcp[q][0]
                break
        RPSV[i] = p
        nn = N
        for q in range(j + 1, len(events)):
            if evlcp[q][1] < LCP[i]:
                nn = evlcp[q][0]
                break
        RNSV[i] = nn
    sigma = _sigma(BWT)
    R = [(-2, 0, False, 2**62)] * sigma
    out = set()
    for i in range(1, N):
        if BWT[i] != BWT[i - 1]:
            for ip in (i - 1, i):
                c = BWT[ip]
                if c != 0:
                    if R[c][0] <= RPSV[i]:
                        if R[c][3] < i:
                            out.add(R[c][1])
                        R[c] = (i, N - sa[ip], True, RNSV[i])
    for c in range(1, sigma):
        if R[c][2]:
            out.add(R[c][1])
    return len(out)


def battery_texts(include_big=True):
    """The 5 texts of parse_probe.py (alphabet 0x0b..0x0e), plus
    random-bin-20k and a 200k scale text."""
    rng = random.Random(20261001)
    out = []
    out.append(("random-4-20k", bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(20000))))
    out.append(("satellite-18k", b"\x0b\x0c\x0d" * 6000 + bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(500))))
    out.append(("HOR-nested", (b"\x0b\x0c\x0d" * 100 + b"\x0e") * 60 + bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(300))))
    paras = [bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(300)) for _ in range(40)]
    docs = []
    for _ in range(50):
        for p in rng.sample(paras, len(paras)):
            docs.append(p)
    out.append(("duplicates-600k", b"".join(docs)[:600000]))
    parts = []
    for i in range(400):
        parts.append(paras[rng.randrange(40)] if rng.random() < 0.5
                     else bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(300)))
    out.append(("dup+unique-120k", b"".join(parts)))
    # random-binary 20k (alphabet 0x0b/0x0c so pfp++ would accept it)
    out.append(("random-bin-20k", bytes(rng.choice((0x0B, 0x0C)) for _ in range(20000))))
    if include_big:
        out.append(("random-4-200k", bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(200000))))
    return out


def fm_texts_battery():
    """The Lean fmTexts battery (all texts over {1,2}, |T| <= 5) + extras,
    mirroring events_probe.py."""
    out = []

    def gen(k, cur):
        if len(cur) == k:
            if cur:
                out.append(bytes(cur))
            return
        gen(k, cur + [1])
        gen(k, cur + [2])

    for L in (1, 2, 3, 4, 5):
        gen(L, [])
    rng = random.Random(20260929)
    out += [bytes(rng.choice((1, 2)) for _ in range(200)) for _ in range(30)]
    out += [bytes(rng.choice((1, 2, 3, 4)) for _ in range(200)) for _ in range(30)]
    out.append(b"\x01\x02\x03" * 100 + bytes(rng.choice((1, 2, 3, 4)) for _ in range(100)))
    out.append(b"\x01\x02" * 150)
    return out


if __name__ == "__main__":
    # validate numpy SA against the gated pure-python SA
    rng = random.Random(7)
    bad = 0
    tot = 0
    for trial in range(40):
        n = rng.randrange(2, 80)
        sig = rng.choice((2, 3, 5))
        x = bytes(rng.randrange(0, sig) for _ in range(n))
        sa_np = suffix_array_np(x)
        sa_py, _ = suffix_array(x)
        tot += 1
        if sa_np != sa_py:
            bad += 1
            print("SA MISMATCH", x, sa_np, sa_py)
    # heavy repetition
    for trial in range(20):
        n = rng.randrange(4, 120)
        x = bytes(rng.choice((1, 1, 1, 2)) for _ in range(n))
        sa_np = suffix_array_np(x)
        sa_py, _ = suffix_array(x)
        tot += 1
        if sa_np != sa_py:
            bad += 1
    print(f"SA numpy validation: {tot-bad}/{tot} match gated pure-python SA")
