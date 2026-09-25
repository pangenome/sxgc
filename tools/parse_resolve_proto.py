#!/usr/bin/env python3
"""parse_resolve_proto.py — Lane 1: row -> text position from PFP artifacts
ALONE (no SA array, no LF-walking, no text scanning).

Ports pfpds::pfp_sa_support (Cvacho/Rossi 2020, vendored in r-pfbwt) to
Python over the raw pfp++ artifacts (.dict, .parse):
  row i -> M-interval (bisect over class boundaries) -> class (len, colex
  range) -> range_select over the wavelet of BWT(P) (colex-leaf order)
  -> saP -> owning phrase occurrence -> phrase-start coordinate ->
  SA[i] = occ_k_next - m.len  (with the cyclic wraparound branch).

Structure sizes (the O(parse+dict)-space claim):
  M classes + boundaries:      O(#dict-suffix-classes) <= O(dict)
  per-class BWT(P) row lists:  total O(n) POSITIONS, but the essential
    wavelet is O(parse log) — the flat lists are a prototype convenience
  saP:                          O(parse)   [int_vector in the real impl]
  phrase starts:                O(parse)
  freq/colex/daD/b_d:           O(dict)

A calibration phase on a small text pins the row/position convention
(cyclic rotations vs linear 0-terminated SA) by brute comparison before
the battery gates run.
"""
import bisect
import os
import random
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from scatter_probe import kasai_lcp, suffix_array

PFP = "/home/erikg/pfp/build/pfp++"
W = 10
P = 100
DOLLAR = 2
ENDOFWORD = 1
ENDOFDICT = 0


def run_pfp(text, prefix):
    for f in os.listdir("/tmp"):
        pass
    open(prefix + ".txt", "wb").write(text)
    r = subprocess.run([PFP, "-t", prefix + ".txt", "-o", prefix, "-w", str(W), "-p", str(P)],
                       capture_output=True, text=True)
    if not os.path.exists(prefix + ".dict") or not os.path.exists(prefix + ".parse"):
        raise RuntimeError("pfp++ failed: " + r.stderr[-400:])


class PFPResolve:
    """All structures built from .dict + .parse only."""

    def __init__(self, prefix, text=None):
        d = open(prefix + ".dict", "rb").read()
        parse = list(struct.unpack(f"<{os.path.getsize(prefix + '.parse') // 4}I",
                                   open(prefix + ".parse", "rb").read()))
        self.w = W
        # --- dictionary string d (port of pfp_ds::dictionary constructor) ---
        n_dollars = 0
        i = 0
        while i < len(d) and d[i] == DOLLAR:
            n_dollars += 1
            i += 1
        d = bytes([DOLLAR]) * (W - n_dollars) + d
        assert d[-1] == ENDOFDICT
        self.d = d
        # phrase starts in d (b_d): ALL ones include the final ENDOFDICT
        # position; n_phrases = rank_b_d(len-1) counts ones in [0, len-1)
        starts_all = [0]
        for i in range(1, len(d)):
            if d[i - 1] == ENDOFWORD:
                starts_all.append(i)
        self.starts_all = starts_all           # select_b_d(x) = starts_all[x-1]
        self.starts_real = [s for s in starts_all if s < len(d) - 1]
        self.n_phrases = len(self.starts_real)
        self.select_b_d = starts_all
        # phrase lengths (length_of_phrase: select(id+1)-select(id)-1)
        self.phrase_len = [starts_all[k + 1] - starts_all[k] - 1 for k in range(self.n_phrases)]
        # --- saD / lcpD / daD ---
        saD, _ = suffix_array(d)
        self.saD = saD
        self.lcpD = kasai_lcp(d, saD, [0] * 0 or None) if False else None
        # need isa: rebuild
        isaD = [0] * len(d)
        for r, p in enumerate(saD):
            isaD[p] = r
        self.lcpD = kasai_lcp(d, saD, isaD)
        daD = []
        sreal = self.starts_real
        for sn in saD:
            out = bisect.bisect_left(sreal, sn)
            if sn in self.starts_set(sreal):
                out += 1
            daD.append(out - 1)
        self.daD = daD
        # --- colex (port of compute_colex_da) ---
        phrases = []
        i = 0
        while i < len(d) - 1:
            j = i
            while j < len(d) - 1 and d[j] != ENDOFWORD:
                j += 1
            phrases.append(bytes(d[i:j]))
            i = j + 1
        assert len(phrases) == self.n_phrases, (len(phrases), self.n_phrases)
        rev = [(p[::-1], k) for k, p in enumerate(phrases)]
        rev.sort(key=lambda t: t[0])  # plain lex on reversed: prefix-first = shorter first
        # colex_id[colex_rank] = lex index; inv_colex_id[lex] = colex_rank
        self.inv_colex = [0] * self.n_phrases
        for cr, (_, lex) in enumerate(rev):
            self.inv_colex[lex] = cr
        colex_daD = [self.inv_colex[self.daD[i] % self.n_phrases] for i in range(len(saD))]
        self.colex_daD = colex_daD
        # --- freq / n ---
        self.parse = parse
        nph1 = self.n_phrases + 1
        freq = [0] * nph1
        for pid in parse:
            freq[pid] += 1
        self.freq = freq
        n = 0
        for pid in parse:
            n += self.phrase_len[pid - 1] - W
        self.n = n
        # --- M + boundaries (port of build_b_bwt_and_M) ---
        self.M = []
        self.boundaries = []
        j = 0
        i = 1
        Nd = len(saD)
        sd = self.starts_all    # select_b_d(x) = sd[x-1]
        sreal = self.starts_real
        while i < Nd:
            left = i
            sn = saD[i]
            phrase = daD[i] + 1
            # suffix_length = select_b_d(rank_b_d(sn+1)+1) - sn - 1
            rk = bisect.bisect_left(sreal, sn + 1)
            nxt = sd[rk]  # next start (incl. the final ENDOFDICT slot)
            suffix_length = nxt - sn - 1
            if (sn in self.starts_set(sreal)) or suffix_length < W:
                i += 1
                continue
            self.boundaries.append(j)
            j += 1
            j += freq[phrase] - 1
            i += 1
            if i < Nd:
                nsn = saD[i]
                nphrase = daD[i] + 1
                rk2 = bisect.bisect_left(sreal, nsn + 1)
                nsuff = sd[rk2] - nsn - 1
                while i < Nd and self.lcpD[i] >= suffix_length and suffix_length == nsuff:
                    j += freq[nphrase]
                    i += 1
                    if i < Nd:
                        nsn = saD[i]
                        nphrase = daD[i] + 1
                        rk2 = bisect.bisect_left(sreal, nsn + 1)
                        nsuff = sd[rk2] - nsn - 1
            right = i - 1
            seg = colex_daD[left:right + 1]
            self.M.append((suffix_length, min(seg), max(seg)))
        # --- saP over p' = parse + [0] ---
        pp = parse + [0]
        saP, _ = suffix_array_arr(pp)
        self.saP = saP
        # --- BWT(P) + colex of its values ---
        bwt_p = [0] * (len(pp) - 1)
        for i in range(1, len(saP)):
            v = pp[saP[i] - 1] if saP[i] > 0 else pp[len(pp) - 2]
            bwt_p[i - 1] = v
        self.bwt_p = bwt_p
        # alphabet_index of value v = colex rank of phrase (v-1)
        self.colex_of_val = [0] * (self.n_phrases + 1)
        for lex in range(self.n_phrases):
            self.colex_of_val[lex + 1] = self.inv_colex[lex]
        colex_bwt = [self.colex_of_val[v] for v in bwt_p]
        # --- wavelet equivalence: per-colex sorted BWT(P) rows (O(parse)) ---
        # A real implementation uses a wavelet tree (O(parse log) space,
        # O(log) per range_select). The prototype keeps the O(parse) base
        # lists and materializes a class's merged row list LAZILY on first
        # query (cost = interval length; total over touched classes only).
        by_colex = {}
        for row, c in enumerate(colex_bwt):
            by_colex.setdefault(c, []).append(row)
        self.by_colex = by_colex
        self._class_rows = {}
        self.materialized_positions = 0
        # --- phrase start positions in the text (select_b_p) ---
        starts = [0]
        acc = 0
        for pid in parse[:-1]:
            acc += self.phrase_len[pid - 1] - W
            starts.append(acc)
        # note: parse[:-1] marks starts of occurrences 1..; select_b_p(x)=starts[x-1]
        self.starts = starts
        self.ops = 0

    @staticmethod
    def starts_set(sd):
        return set(sd)

    def class_rows(self, cls):
        if cls not in self._class_rows:
            l, r = self.M[cls][1], self.M[cls][2]
            rows = []
            for c in range(l, r + 1):
                rows.extend(self.by_colex.get(c, []))
            rows.sort()
            self._class_rows[cls] = rows
            self.materialized_positions += len(rows)
        return self._class_rows[cls]

    def resolve(self, i):
        """SA[i] from parse coordinates only."""
        self.ops += 1
        # rank_i = # boundaries <= i; lex_rank_i = rank_i - 1
        rank_i = bisect.bisect_right(self.boundaries, i)
        cls = rank_i - 1
        if cls < 0 or cls >= len(self.M):
            raise IndexError(f"row {i} outside M intervals")
        interval_rank = i - self.boundaries[rank_i - 1]
        mlen, l, r = self.M[cls]
        rows = self.class_rows(cls)
        if interval_rank >= len(rows):
            raise IndexError(f"row {i}: interval_rank {interval_rank} >= {len(rows)}")
        k = rows[interval_rank]  # range_select(l, r, interval_rank+1)
        # p_i from saP
        sp = self.saP[k + 1]
        if sp > 0:
            p_i = sp - 1
        else:
            p_i = len(self.parse) - 1  # p.size()-2 with p' = parse+[0] => len(parse)-1
        # occ_k_next = select_b_p(p_i + 2) or n
        if p_i + 2 > len(self.parse):  # p.size()-1 with p' = parse+[0]
            occ_k_next = self.n
        else:
            occ_k_next = self.starts[p_i + 1]
        if occ_k_next < mlen:
            return self.n - (mlen - occ_k_next)
        return occ_k_next - mlen


def suffix_array_arr(x):
    """SA of an int list (0 = smallest), dense tie-aware (same as scatter_probe)."""
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
        for i2 in range(1, n):
            tmp[sa[i2]] = tmp[sa[i2 - 1]] + (key(sa[i2]) != key(sa[i2 - 1]))
        rank = tmp[:]
        if rank[sa[-1]] == n - 1:
            break
        k *= 2
    return sa, rank


def brute_cyclic(T):
    n = len(T)
    return sorted(range(n), key=lambda i: (T + T)[i:i + n])


def brute_linear(T):
    X = T + b"\x00"
    sa, _ = suffix_array(X)
    return sa


def calibrate_and_gate(T, name, prefix, full_check_limit=200000):
    """CONVENTION (empirically pinned on random-4 n=1500, then verified here):
    machinery text = T + $^w (linear, no terminator row); n = |T| + w;
    rows 0..w-1 = the dollar suffixes (positions [|T|, |T|+w));
    machinery row i >= w  <=>  linear SA row (i - w + 1) of X = T + \x00,
    and its resolved position == the T-position directly.
    GATE: (a) all machinery-BWT run-boundary rows, (b) 100 random rows.
    """
    run_pfp(T, prefix)
    R = PFPResolve(prefix)
    n = R.n
    if n != len(T) + W:
        return ("FAIL", f"n={n} != |T|+w={len(T)+W}")
    # resolve ALL rows (prototype scale) to build the machinery BWT
    pos = [R.resolve(i) for i in range(n)]
    Tp = T + bytes([DOLLAR]) * W
    bwt = [Tp[pos[i] - 1] if pos[i] > 0 else Tp[-1] for i in range(n)]
    # sanity: rows 0..w-1 must be the dollar suffixes
    for i in range(W):
        if pos[i] < len(T):
            return ("FAIL", f"row {i} not a dollar row: pos={pos[i]}")
    # run-boundary rows of the machinery BWT
    bnd_rows = [i for i in range(1, n) if bwt[i] != bwt[i - 1]]
    rng = random.Random(20260925)
    rand_rows = rng.sample(range(n), 100)
    # brute linear SA of X = T + \x00
    lin, _ = suffix_array(T + b"\x00")
    # verify: (a) all rows on small texts; (b) boundary + 100 random rows on big
    check_rows = list(range(n)) if n <= full_check_limit else sorted(set(bnd_rows + rand_rows))
    bad = 0
    for i in check_rows:
        if i < W:
            continue
        want = lin[i - W + 1]
        if pos[i] != want:
            bad += 1
            if bad <= 3:
                print(f"    mismatch row {i}: got {pos[i]} want {want}")
    # also verify the dollar rows are a permutation of [|T|, |T|+w)
    dollar_ok = sorted(pos[:W]) == list(range(len(T), len(T) + W))
    verdict = "PASS" if (bad == 0 and dollar_ok) else "FAIL"
    n_ops = R.ops
    return (verdict, f"rows-checked={len(check_rows)} bad={bad} dollar_ok={dollar_ok} "
            f"resolve-ops={n_ops} |parse|={len(R.parse)} |dict-phrases|={R.n_phrases} "
            f"|M|={len(R.M)} class-list-bytes={R.materialized_positions}")


def main():
    rng = random.Random(20261001)
    os.makedirs("/tmp/prp", exist_ok=True)
    os.chdir("/tmp/prp")
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
    allpass = True
    for name, T in texts:
        print(f"[{name}] n={len(T)}")
        verdict, msg = calibrate_and_gate(T, name, "b_" + name)
        print(f"  {verdict}: {msg}")
        if verdict != "PASS":
            allpass = False
    print("PARSE-RESOLVE GATE:", "ALL PASS" if allpass else "FAILURES PRESENT")
    print("PROTO DONE")


if __name__ == "__main__":
    main()
