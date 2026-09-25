#!/usr/bin/env python3
"""endpoint_common.py — shared loader for the ENDPOINT-RULE lane.

Conventions (all pinned by the gated tools; verified again on load):
  * linear space: X = T + b'\\x00', N = |T|+1 rows; brute SA/LCP/BWT.
  * PFP machinery: PFPResolve over T + $^W; machinery row m >= W <->
    linear row m - W + 1; positions identical; resolve_lin(0) = |T|.
  * class blocks (the row partition induced by the dictionary-suffix
    classes M): machinery rows partition into contiguous blocks
    [boundaries[j], boundaries[j+1]); clipped to linear rows [1, N).
  * row 0 is its own pseudo-block with LCP[0] = 0 (sentinel for PSV).
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from scatter_probe import kasai_lcp, suffix_array, sv  # gated
from parse_resolve_proto import PFPResolve, W, run_pfp  # gated


def battery():
    """The 7-text battery (alphabet 0x0b..0x0e, pfp++-safe), same seeds
    as c_probe_common.battery_texts so every number is comparable."""
    import random
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
    out.append(("random-bin-20k", bytes(rng.choice((0x0B, 0x0C)) for _ in range(20000))))
    out.append(("random-4-200k", bytes(rng.choice((0x0B, 0x0C, 0x0D, 0x0E)) for _ in range(200000))))
    return out


class TextCase:
    """Brute structures + PFP machinery + the linear class partition."""

    def __init__(self, name, T, workdir="/tmp/endpoint"):
        os.makedirs(workdir, exist_ok=True)
        self.name = name
        self.T = T
        self.N = len(T) + 1
        self.X = T + b"\x00"
        # brute ground truth (gate only)
        sa, _ = suffix_array(self.X)
        self.sa = sa
        self.isa = [0] * self.N
        for i, p in enumerate(sa):
            self.isa[p] = i
        self.LCP = kasai_lcp(self.X, sa, self.isa)
        self.BWT = [self.X[sa[i] - 1] if sa[i] else 0 for i in range(self.N)]
        self.psvL, self.nsvL = sv(self.LCP)
        # runs
        self.runs = []
        st = 0
        for i in range(1, self.N):
            if self.BWT[i] != self.BWT[i - 1]:
                self.runs.append((self.BWT[i - 1], st, i - 1))
                st = i
        self.runs.append((self.BWT[self.N - 1], st, self.N - 1))
        self.run_id = [0] * self.N
        for ri, (c, a, b) in enumerate(self.runs):
            for j in range(a, b + 1):
                self.run_id[j] = ri
        # PLCP pieces (direct law)
        PLCP = [self.LCP[self.isa[p]] for p in range(self.N)]
        self.PLCP = PLCP
        self.piece_start = [0]
        self.piece_sample = [PLCP[0]]
        for p in range(1, self.N):
            if PLCP[p - 1] != PLCP[p] + 1:
                self.piece_start.append(p)
                self.piece_sample.append(PLCP[p])
        # PFP machinery
        prefix = os.path.join(workdir, "e_" + name)
        run_pfp(T, prefix)
        self.R = PFPResolve(prefix)
        assert self.R.n == len(T) + W, (self.R.n, len(T) + W)
        # --- linear class partition for rows [1, N) ---
        # machinery row m = l + W - 1; block(j) via bisect on boundaries.
        Rb = self.R.boundaries
        self.blocks = []       # list of (lo, hi) linear rows, contiguous
        # pseudo-block 0: row 0 alone
        self.blocks.append((0, 1))
        # machinery block boundaries -> linear, clipped to [1, N)
        lin_bounds = [max(b - W + 1, 1) for b in Rb] + [self.R.n - W + 1]
        # dedupe/clip
        for j in range(len(Rb)):
            lo = lin_bounds[j]
            hi = lin_bounds[j + 1] if j + 1 < len(lin_bounds) else self.R.n - W + 1
            hi = min(hi, self.N)
            if lo < hi:
                if self.blocks and self.blocks[-1][1] == lo:
                    pass  # contiguous
                self.blocks.append((lo, hi))
        # contiguity fix: force blocks to tile [0, N) exactly
        fixed = [self.blocks[0]]
        for (lo, hi) in self.blocks[1:]:
            if lo < fixed[-1][1]:
                lo = fixed[-1][1]
            if lo < hi:
                if lo > fixed[-1][1]:
                    # a gap (dollar/clip artifacts): glue into previous block
                    fixed[-1] = (fixed[-1][0], hi)
                else:
                    fixed.append((lo, hi))
        self.blocks = fixed
        if self.blocks[-1][1] < self.N:
            self.blocks[-1] = (self.blocks[-1][0], self.N)
        self.block_of = [0] * self.N
        for bi, (lo, hi) in enumerate(self.blocks):
            for j in range(lo, hi):
                self.block_of[j] = bi
        self.block_mlen = [0] * len(self.blocks)
        for bi, (lo, hi) in enumerate(self.blocks):
            if lo >= 1:
                m = lo + W - 1
                bj = bisect.bisect_right(Rb, m) - 1
                if 0 <= bj < len(self.R.M):
                    self.block_mlen[bi] = self.R.M[bj][0]
                else:
                    self.block_mlen[bi] = -1
        # resolve interface
        self._res_ops = 0

    def resolve_lin(self, j):
        if j == 0:
            return len(self.T)
        self._res_ops += 1
        return self.R.resolve(j + W - 1)

    def plcp_of_row(self, j):
        """LCP[j] the r-space way: piece law at the resolved position."""
        pos = self.resolve_lin(j)
        i = bisect.bisect_right(self.piece_start, pos) - 1
        return self.piece_sample[i] - (pos - self.piece_start[i])
