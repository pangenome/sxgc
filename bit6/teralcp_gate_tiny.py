#!/usr/bin/env python3
"""Gate 4a.1-tiny (fresh): TeraLCP thresholds vs brute force, full production path.

Generates a crafted 30-string DNA collection (duplicate strings, prefix pairs,
homopolymers, palindromes), runs the production chain
    text -> grlBWT -> grlbwt2rle -> 5-byte .bwt.len -> TeraLCP (-f rlbwt)
and brute-forces thresholds from the known text under the grlBWT/BCR convention
(documented + 712/712-validated in rlbwt_sampler.cpp):
    * multi-string BWT: rows 0..k-1 are the bare sentinels $1<...<$k, all
      sentinels sort before the alphabet, ordered by string index;
    * equivalent flat text: S1 chr(1) S2 chr(2) ... Sk chr(k) — suffix sort on
      this reproduces the multi-string SA and LCP exactly;
    * TeraLCP splits multi-endmarker '\n' runs so each endmarker is its own
      run; thresholds are per split run:
        first run of char c               -> (thr=0, pos=0)
        run i of c starting at BWT row s,
        previous c-run ending at row p    -> thr = min{lcp[k] : p < k <= s},
                                               pos = earliest argmin (default
                                               mode, no -threshbound)
Exit 0 iff thr and thr_pos match entry-for-entry.
"""
import os, random, struct, subprocess, sys

GATE = os.environ.get("GATE_DIR", "/tmp/grl_gate")
GRL = "/home/erikg/grlBWT/build/grlbwt-cli"
RLE = "/home/erikg/grlBWT/build/grlbwt2rle"
TLC = "/home/erikg/TeraTools/src/TeraLCP/TeraLCP"

def gen_collection():
    rng = random.Random(20260920)
    strs = []
    def dna(n): return "".join(rng.choice("ACGT") for _ in range(n))
    strs.append("A" * 40)                       # homopolymer
    strs.append("A" * 40)                       # duplicate of it (ties)
    strs.append("AC" * 25)                      # alternating
    strs.append("AC" * 25)                      # duplicate
    strs.append("A" * 12 + "CGT")               # prefix-ish overlaps
    strs.append("A" * 12 + "CGTA")              # extends the previous
    strs.append("CGTACGTACGTACGTAAAATT")        # periodic + tail
    strs.append("TTTTTTTTTTGGGGGGGGGG")         # runs
    strs.append("GATTACAGATTACAGATTACA")        # repeats
    strs.append("".join(dna(3) for _ in range(7)))  # trin-repetitive
    while len(strs) < 30:
        strs.append(dna(rng.randint(20, 300)))
    return strs

def brute_thresholds(text_flat, n, sent_vals):
    """Multi-string brute force on the flat text with distinct sentinels."""
    # suffix sort with sentinel mapping: '\n' i-th occurrence -> chr(i+1)
    mp = bytearray(text_flat)
    seq = 0
    for i, c in enumerate(mp):
        if c == 0x0A:
            mp[i] = sent_vals[seq]
            seq += 1
    mp = bytes(mp)
    sa = sorted(range(len(mp)), key=lambda i: mp[i:])
    bwt = bytes(mp[(sa[i] - 1) % len(mp)] for i in range(len(mp)))
    # map sentinel chars back to '\n' for comparison with grlBWT output
    bwt = bytes(0x0A if c in sent_vals else c for c in bwt)
    # LCP (Kasai)
    n2 = len(mp)
    rank = [0]*n2
    for i, s in enumerate(sa): rank[s] = i
    lcp = [0]*n2
    h = 0
    for i in range(n2):
        if rank[i] > 0:
            j = sa[rank[i]-1]
            while i+h < n2 and j+h < n2 and mp[i+h] == mp[j+h]: h += 1
            lcp[rank[i]] = h
            if h: h -= 1
        else:
            h = 0
    # split runs of the BWT (endmarkers one per run), thresholds
    runs = []
    i = 0
    while i < n2:
        j = i
        while j < n2 and bwt[j] == bwt[i]: j += 1
        if bwt[i] == 0x0A:
            for k in range(i, j): runs.append((0x0A, k, k))
        else:
            runs.append((bwt[i], i, j-1))
        i = j
    thr, pos = [], []
    last_end, seen = {}, set()
    for c, s, e in runs:
        if c not in seen:
            thr.append(0); pos.append(0); seen.add(c); last_end[c] = e; continue
        p = last_end[c]
        best, bpos = None, None
        for k in range(p+1, s+1):
            if best is None or lcp[k] < best:
                best, bpos = lcp[k], k
        thr.append(best); pos.append(bpos)
        last_end[c] = e
    return bwt, runs, thr, pos

def main():
    strs = gen_collection()
    os.makedirs(GATE, exist_ok=True)
    os.chdir(GATE)
    prefix = "ft30"
    text = ("\n".join(strs) + "\n").encode()
    open(f"{prefix}.txt", "wb").write(text)
    k = len(strs)
    n = len(text)
    print(f"{prefix}: k={k} strings, flat n={n}")

    # production chain
    env = dict(os.environ, TMPDIR=GATE)
    for f in (f"{prefix}.rl_bwt", f"{prefix}t.syms", f"{prefix}t.len",
              f"{prefix}t.bwt.heads", f"{prefix}t.bwt.len"):
        if os.path.exists(f): os.remove(f)
    r = subprocess.run([GRL, f"{prefix}.txt", "-t", "8", "-T", GATE],
                       capture_output=True, text=True, env=env)
    if r.returncode != 0:
        print("grlbwt FAILED:", r.stderr[-500:]); sys.exit(1)
    r = subprocess.run([RLE, f"{prefix}.rl_bwt", f"{prefix}t"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("grlbwt2rle FAILED:", r.stderr[-500:]); sys.exit(1)
    # 5-byte conversion
    syms = open(f"{prefix}t.syms", "rb").read()
    lens4 = open(f"{prefix}t.len", "rb").read()
    R = len(syms)
    vals = struct.unpack(f"<{R}I", lens4[:R*4])
    with open(f"{prefix}t.bwt.heads", "wb") as f: f.write(syms)
    with open(f"{prefix}t.bwt.len", "wb") as f:
        for v in vals: f.write(v.to_bytes(5, "little"))
    open(f"{prefix}t.scratch", "wb").close()
    print(f"grlBWT: R={R} runs, n={sum(vals)}")

    # brute force under BCR convention
    sent_vals = [i+1 for i in range(k)]
    bwt_b, runs_b, thr_b, pos_b = brute_thresholds(text, n, sent_vals)
    # expand production BWT and compare (pipeline re-validation)
    bwt_g = bytearray()
    for c, v in zip(syms, vals): bwt_g += bytes([c])*v
    if bytes(bwt_g) != bwt_b:
        d = [i for i in range(n) if bwt_g[i] != bwt_b[i]]
        print(f"BWT MISMATCH with brute force: {len(d)} rows, first {d[:5]}")
        sys.exit(1)
    print(f"BWT re-validated: {R} runs, {n} rows byte-identical vs brute force")

    # TeraLCP
    for f in ("thr.thr", "thr.thr_pos", f"{prefix}t.lcp_index.lcp_index"):
        if os.path.exists(f): os.remove(f)
    r = subprocess.run([TLC, "-f", "rlbwt", "-i", f"{prefix}t", "-t", f"{prefix}t.scratch",
                        "-oindex", f"{prefix}t.lcp_index", "-othresholds", "thr",
                        "--thr-pfp"], capture_output=True, text=True)
    if r.returncode != 0:
        print("TeraLCP FAILED:", r.returncode, r.stderr[-2000:]); sys.exit(1)
    g_thr_b = open("thr.thr", "rb").read()
    g_pos_b = open("thr.thr_pos", "rb").read()
    m = len(g_thr_b)//5
    g_thr = [int.from_bytes(g_thr_b[i*5:(i+1)*5], "little") for i in range(m)]
    g_pos = [int.from_bytes(g_pos_b[i*5:(i+1)*5], "little") for i in range(m)]
    print(f"TeraLCP entries={m}, brute split-runs={len(thr_b)}")
    if m != len(thr_b):
        print("ENTRY COUNT MISMATCH"); sys.exit(1)
    bad = [(i, g_thr[i], thr_b[i], g_pos[i], pos_b[i]) for i in range(m)
           if g_thr[i] != thr_b[i] or g_pos[i] != pos_b[i]]
    nval = sum(1 for i in range(m) if g_thr[i] != thr_b[i])
    npos = sum(1 for i in range(m) if g_pos[i] != pos_b[i])
    print(f"value mismatches: {nval}/{m}, position mismatches: {npos}/{m}")
    if bad:
        for i, gt, bt, gp, bp in bad[:15]:
            print(f"  [{i}] thr tera={gt} brute={bt}  pos tera={gp} brute={bp}")
        sys.exit(1)
    print(f"GATE GREEN: {m}/{m} threshold value+position entries byte-verified")

if __name__ == "__main__":
    main()
