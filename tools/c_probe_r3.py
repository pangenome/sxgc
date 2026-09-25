#!/usr/bin/env python3
"""c_probe_r3.py — R3: THE INVERSE MAP (position -> row) from PFP artifacts.

Derivation (from sa_support.hpp + the prototype's forward map):
  forward:  row -> class j (rank on b_bwt) -> interval_rank -> k = wavelet
            range_select(M[j].left,M[j].right,interval_rank+1) -> p_i =
            saP[k+1]-1 -> occ_k_next = starts[p_i+1] -> pos = occ_k_next - M[j].len
  inverse:  let occ = the occurrence containing pos (starts is sorted; and the
            forward identity pos = starts[p_i+1] - M[j].len with M[j].len <=
            starts[p_i+1]-starts[p_i] forces p_i = occ).  Then:
              m.len = starts[occ+1] - pos           (O(1))
              ds    = select_b_d(phrase(occ)) + W + off,  off = pos-starts[occ]
              j     = cls_of_ds[ds]                 (O(dict) lookup table)
              k     = isaP[occ+1] - 1               (O(parse) inverse of saP)
              row   = boundaries[j] + bisect_left(class_rows(j), k)
All structures O(parse) / O(dict) space; each query O(log).
GATE: resolve_inv(resolve(row)) == row for every machinery row.
"""
import bisect
import os
import sys

sys.path.insert(0, "tools")
from c_probe_common import battery_texts
from parse_resolve_proto import PFPResolve, run_pfp


class InvResolve(PFPResolve):
    """PFPResolve + the inverse structures."""

    def __init__(self, prefix):
        super().__init__(prefix)
        # --- isaP: inverse of saP (saP is SA of pp = parse + [0]) ---
        self.isaP = [0] * len(self.saP)
        for r, p in enumerate(self.saP):
            self.isaP[p] = r
        # --- cls_of_ds: dictionary suffix start -> class index (M index) ---
        # replicate the M-build loop; two counters: cls_idx (M index) and
        # row_off (row start of the class block, matching `boundaries`).
        saD, lcpD, daD = self.saD, self.lcpD, self.daD
        sd = self.select_b_d
        sreal = self.starts_real
        sreal_set = set(sreal)
        self.cls_of_ds = {}
        cls_idx = 0
        row_off = 0
        i = 1
        Nd = len(saD)
        while i < Nd:
            left = i
            sn = saD[i]
            phrase = daD[i] + 1
            rk = bisect.bisect_left(sreal, sn + 1)
            nxt = sd[rk]
            suffix_length = nxt - sn - 1
            if (sn in sreal_set) or suffix_length < self.w:
                i += 1
                continue
            row_off += 1
            row_off += self.freq[phrase] - 1
            i += 1
            if i < Nd:
                nsn = saD[i]
                nphrase = daD[i] + 1
                rk2 = bisect.bisect_left(sreal, nsn + 1)
                nsuff = sd[rk2] - nsn - 1
                while i < Nd and lcpD[i] >= suffix_length and suffix_length == nsuff:
                    row_off += self.freq[nphrase]
                    i += 1
                    if i < Nd:
                        nsn = saD[i]
                        nphrase = daD[i] + 1
                        rk2 = bisect.bisect_left(sreal, nsn + 1)
                        nsuff = sd[rk2] - nsn - 1
            right = i - 1
            # THE FIRST sn of the group is the class representative; extra sn's
            # (same suffix up to terminator, different phrases) also map here.
            for t in range(left, right + 1):
                self.cls_of_ds[saD[t]] = cls_idx
            cls_idx += 1

    def resolve_inv(self, pos):
        """position -> machinery row (pinned conventions).

        Pinned rules (all from the forward map + brute gate):
          * positions >= |T| are the w dollar rows: row = pos - |T|.
          * the row's suffix context ends at the first phrase-occurrence
            start s > pos with s - pos >= W (s = n if none): mlen = s - pos.
            (Near-end-of-phrase positions skip boundaries closer than W —
            the build loop only creates classes for suffix length >= W.)
          * occ p_i = (s's occurrence index) - 1   [s = starts[p_i+1]]
          * dictionary offset: ds = select_b_d(pid) + (len_phrase(pid) - mlen)
            with pid = parse[p_i]  (equivalently + W + (pos - starts[p_i]) when
            the linear offset is non-negative).
          * class j = cls_of_ds[ds];
          * k = isaP[p_i+1] - 1, except p_i = |parse|-1 -> k = isaP[0] - 1;
          * row = boundaries[j] + rank of k in class_rows(j).
        """
        W = self.w
        nT = self.n - W
        if pos >= nT:
            return pos - nT, None
        # first occurrence start > pos at distance >= W (or n)
        o = bisect.bisect_right(self.starts, pos) - 1          # containing occ
        s = None
        for k_occ in range(o + 1, len(self.starts)):
            if self.starts[k_occ] - pos >= W:
                s = self.starts[k_occ]
                break
        if s is None:
            if self.n - pos >= W:
                s = self.n
                p_i = len(self.parse) - 1
            else:
                return None, f"no valid boundary (pos={pos})"
        else:
            p_i = k_occ - 1                                    # starts[p_i+1] = s
        mlen = s - pos
        pid = self.parse[p_i]
        lphrase = self.phrase_len[pid - 1]  # dictionary phrase length (span + W)
        ds = self.select_b_d[pid - 1] + (lphrase - mlen)
        cls = self.cls_of_ds.get(ds)
        if cls is None or self.M[cls][0] != mlen:
            return None, f"no class (pos={pos} p_i={p_i} pid={pid} mlen={mlen} ds={ds})"
        k = self.isaP[0] - 1 if p_i == len(self.parse) - 1 else self.isaP[p_i + 1] - 1
        rows = self.class_rows(cls)
        ir = bisect.bisect_left(rows, k)
        if ir >= len(rows) or rows[ir] != k:
            return None, f"k={k} not in class {cls} rows (pos={pos} p_i={p_i})"
        return self.boundaries[cls] + ir, None


def main():
    rng = __import__("random").Random(20261001)
    os.makedirs("/tmp/cpr3", exist_ok=True)
    os.chdir("/tmp/cpr3")
    from c_probe_common import battery_texts
    texts = battery_texts()   # the aligned battery (0x0b..0x0e)
    if len(sys.argv) > 1 and sys.argv[1] == "small":
        texts = [t for t in texts if t[0] in ("random-4-20k", "satellite-18k")]
    for name, T in texts:
        run_pfp(T, "b_" + name)
        R = InvResolve("b_" + name)
        n = R.n
        print(f"[{name}] n={n} |parse|={len(R.parse)} |M|={len(R.M)} |cls_of_ds|={len(R.cls_of_ds)}")
        ok = bad = skip = 0
        first_err = None
        for i in range(n):
            pos = R.resolve(i)
            r2, err = R.resolve_inv(pos)
            if r2 is None:
                skip += 1
                if first_err is None:
                    first_err = (i, pos, err)
            elif r2 == i:
                ok += 1
            else:
                bad += 1
                if first_err is None:
                    first_err = (i, pos, r2)
        print(f"  INVERSE GATE: ok={ok} bad={bad} unresolved={skip} / rows={n}")
        if first_err:
            print("  first failure:", first_err)
    print("R3 DONE")


if __name__ == "__main__":
    main()
