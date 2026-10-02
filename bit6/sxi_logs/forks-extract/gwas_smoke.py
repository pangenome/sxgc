#!/usr/bin/env python3
"""Gate (c) GWAS smoke: yeast fork matrix + simulated phenotypes (v3).

Panel: columns (emitted records) sampled by random seeks from the full yeast
matrix, restricted to informative ones (>=3 of the 100 study strings).
Causal loci: 20 columns with DISTINCT class string-sets (sizes 15..40), each
given an INDEPENDENT random-signed effect eps_i (|eps| in [0.5, 1.5]) so no
compound/union column can out-track a locus's own signal. Phenotype per
string: case with probability sigma(beta * sum_i eps_i * [string in class_i]).
Every panel column gets a 1-df chi-square (pure python: p = erfc(sqrt(chi2/2))).
Reported: empirical null calibration, causal-rank enrichment vs random
columns, Bonferroni detections, and LD-twin accounting across replicates and
effect sizes.
"""
import json
import math
import os
import random
import sys


def sample_lines(path, count, seed):
    size = os.path.getsize(path)
    rng = random.Random(seed)
    out = []
    for _ in range(count):
        off = rng.randrange(0, max(1, size - 4096))
        with open(path, "rb") as f:
            f.seek(off)
            f.readline()
            line = f.readline()
        if not line.endswith(b"\n"):
            with open(path, "rb") as f:
                f.seek(off)
                f.readline()
                line = f.readline() + f.read(1 << 16)
                line = line[: line.find(b"\n") + 1]
        try:
            out.append(json.loads(line))
        except Exception:
            pass
    return out


def chi2_p(a, b, c, d):
    n = a + b + c + d
    if n == 0 or 0 in (a + b, c + d, a + c, b + d):
        return 1.0
    e = [(a + b) * (a + c) / n, (a + b) * (b + d) / n,
         (c + d) * (a + c) / n, (c + d) * (b + d) / n]
    stat = sum((x - y) ** 2 / y for x, y in zip((a, b, c, d), e))
    return math.erfc(math.sqrt(stat / 2.0))


def main(path, nstrings=100, npanel=50000, nloci=20, replicates=10, seed=7):
    rng = random.Random(seed)
    strings = list(range(nstrings))
    sset = set(strings)

    print(f"sampling ~{npanel} columns from {path} ...")
    cols = sample_lines(path, npanel, seed)
    dedup = {}
    for o in cols:
        hits = frozenset(s for s in o["strings"] if s in sset)
        if len(hits) >= 3:
            dedup[(o["witness"], o["class_id"])] = hits
    panel = [(w, cid, hits) for (w, cid), hits in dedup.items()]
    print(f"informative panel: {len(panel)} columns (>=3 of {nstrings} strings)")

    causal, seen_sets = [], set()
    for w, cid, hits in panel:
        if 15 <= len(hits) <= 40 and hits not in seen_sets:
            causal.append((w, cid, hits))
            seen_sets.add(hits)
        if len(causal) >= nloci:
            break
    eps = {i: rng.choice([-1, 1]) * rng.uniform(0.5, 1.5) for i in range(len(causal))}
    print(f"causal loci: {len(causal)} DISTINCT string-sets, sizes "
          f"{[len(h) for _,_,h in causal]}, signed effects {list(eps.values())[:6]}...")
    # LD twins in the panel for each causal set
    twins = [sum(1 for _,_,p in panel
                 if p != h and len(p & h)/len(p | h) >= 0.8)
             for _,_,h in causal]
    print(f"per-locus near-twins (Jaccard>=0.8) in panel: {twins}")

    risk = {s: sum(eps[i] for i, (_, _, h) in enumerate(causal) if s in h)
            for s in strings}

    # empirical null: permuted phenotypes
    null_ps = []
    for _, _, hits in panel:
        perm = strings[:]
        rng.shuffle(perm)
        cases = set(perm[:50])
        a = sum(1 for s in hits if s in cases)
        b = len(hits) - a
        null_ps.append(chi2_p(a, b, 50 - a, 50 - b))
    null_ps.sort()
    n05 = sum(1 for p in null_ps if p < 0.05)
    print(f"empirical null ({len(panel)} permuted columns): median p "
          f"{null_ps[len(null_ps)//2]:.3g}, p<0.05 {n05} ({100*n05/len(null_ps):.1f}%)")

    alpha = 0.05 / len(panel)
    print(f"Bonferroni alpha over {len(panel)} columns: {alpha:.3g}")

    # ---- single-locus power: the clean per-locus recovery measurement ----
    # For each causal locus, simulate phenotypes associated ONLY at that locus
    # (P(case) = 0.75 in class, 0.25 out), test the whole panel, and record the
    # locus's rank/p-value against the null columns.
    delta = 0.25
    sig = 0
    medr = []
    for w, cid, hits in causal:
        for rep in range(5):
            pheno = {s: (1 if rng.random() < (0.5 + (delta if s in hits else -delta)) else 0)
                     for s in strings}
            ncase = sum(pheno.values())
            results = []
            for pw, pc, ph in panel:
                a = sum(1 for s in ph if pheno[s] == 1)
                b = len(ph) - a
                c = ncase - a
                d = (nstrings - ncase) - b
                results.append((chi2_p(a, b, c, d), pw, pc))
            results.sort()
            rank = next(i for i, (_, pw, pc) in enumerate(results)
                        if (pw, pc) == (w, cid)) + 1
            p = results[rank - 1][0]
            medr.append(rank)
            if p < alpha:
                sig += 1
    trials = len(causal) * 5
    print(f"\nsingle-locus power (delta=±{delta}, {trials} locus-replicates): "
          f"Bonferroni-significant {sig}/{trials}; "
          f"median causal rank {sorted(medr)[len(medr)//2]}/{len(panel)} "
          f"(random expectation {len(panel)//2}); "
          f"best rank {min(medr)}")

    for beta in (0.6, 1.0, 1.5):
        detected = 0
        in20 = 0
        in100 = 0
        medranks = []
        for rep in range(replicates):
            pheno = {s: (1 if rng.random() < 1/(1+math.exp(-beta*risk[s])) else 0)
                     for s in strings}
            ncase = sum(pheno.values())
            results = []
            for w, cid, hits in panel:
                a = sum(1 for s in hits if pheno[s] == 1)
                b = len(hits) - a
                c = ncase - a
                d = (nstrings - ncase) - b
                results.append((chi2_p(a, b, c, d), w, cid))
            results.sort()
            keyrank = {(w, cid): i for i, (_, w, cid) in enumerate(results)}
            ranks = sorted(keyrank[(w, c)] + 1 for w, c, _ in causal)
            medranks.append(ranks[len(ranks)//2])
            detected += sum(1 for p, w, cid in results
                            if (w, cid) in set((w2, c2) for w2, c2, _ in causal) and p < alpha)
            in20 += sum(1 for r in ranks if r <= 20)
            in100 += sum(1 for r in ranks if r <= 100)
        n = len(panel)
        print(f"beta={beta}: causal median-rank {sorted(medranks)[len(medranks)//2]}/{n} "
              f"(random expectation {n//2}); in top-20: {in20/replicates:.1f}/20 "
              f"(chance {20*20/n:.2f}); in top-100: {in100/replicates:.1f}/20 "
              f"(chance {100*20/n:.2f}); Bonferroni: {detected/replicates:.1f}/20")

    # one top table at beta=1.5
    beta = 1.5
    pheno = {s: (1 if rng.random() < 1/(1+math.exp(-beta*risk[s])) else 0) for s in strings}
    ncase = sum(pheno.values())
    results = []
    for w, cid, hits in panel:
        a = sum(1 for s in hits if pheno[s] == 1)
        b = len(hits) - a
        c = ncase - a
        d = (nstrings - ncase) - b
        results.append((chi2_p(a, b, c, d), w, cid))
    results.sort()
    ckeys = {(w, cid) for w, cid, _ in causal}
    print(f"\ntop 20 (beta=1.5, {ncase}/{nstrings} cases):")
    for i, (p, w, cid) in enumerate(results[:20]):
        print(f"  {i+1:3d}  p={p:.3g}  witness={w} class={cid} "
              f"{'CAUSAL' if (w, cid) in ckeys else ''}")


if __name__ == "__main__":
    main(sys.argv[1])
