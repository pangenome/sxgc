#!/usr/bin/env python3
"""Gate 4a.1-tiny: TeraLCP thresholds vs brute force on the t10 collection.

t10.syms/t10.len are the grlBWT BCR runs (gated 712/712 vs brute force earlier).
This gate:
  1. expands the RLE BWT and inverts it via LF mapping to recover the text
     (grlBWT FMD convention: '\n' endmarkers sort before the alphabet),
  2. brute-forces SA (full suffix sort) + LCP (Kasai),
  3. computes thresholds per SPLIT run (each endmarker in its own run):
       for run i of char c with start row s_i, previous c-run end row p_i:
         first run of c          -> (thr=0, pos=0)
         else thr = min{ LCP[k] : p_i < k <= s_i }, pos = earliest argmin
  4. runs TeraLCP on the same input (5-byte .bwt.len format) and byte-compares
     thr.thr and thr.thr_pos (default earliest-min position; no -threshbound).
Exit 0 iff all entries match.
"""
import struct, subprocess, sys, os, shutil

GATE = "/tmp/grl_gate"
TLC = "/home/erikg/TeraTools/src/TeraLCP/TeraLCP"

def read_runs(prefix):
    syms = open(f"{prefix}.syms", "rb").read()
    lens = open(f"{prefix}.len", "rb").read()
    n4 = os.path.getsize(f"{prefix}.len") // 4
    vals = struct.unpack(f"<{n4}I", lens[:n4*4])
    return syms, vals

def expand_bwt(syms, lens):
    bwt = bytearray()
    for c, l in zip(syms, lens):
        bwt += bytes([c]) * l
    return bytes(bwt)

def invert_bwt(bwt):
    """Standard LF inversion with '\n' as smallest (grlBWT convention)."""
    n = len(bwt)
    occ = [0]*256
    for c in bwt: occ[c] += 1
    # F offsets ('\n' smallest, then lexicographic)
    alpha = sorted(set(bwt))
    F = {}
    tot = 0
    for c in alpha:
        F[c] = tot; tot += occ[c]
    # LF: rank within char + F offset
    cnt = [0]*256
    lf = [0]*n
    for i, c in enumerate(bwt):
        lf[i] = F[c] + cnt[c]; cnt[c] += 1
    # find last row of the cyclic rotation = row whose BWT char is '\n' with the
    # largest LF among endmarkers (row i is last iff BWT[i]=='\\n' selects the
    # full sequence ending there); walk back from that row.
    # Row 0 in FMD with '\n' smallest: first '\n' in BWT order... simplest:
    # pick the row j such that LF(j) is the largest '\n' row -> that LF row is
    # the first row (row 0). Then text = walk forward n rows via inverse LF.
    nl_rows = [i for i, c in enumerate(bwt) if c == 0x0A]
    assert nl_rows, "no endmarkers in BWT"
    # LF(row with '\n') points at the row that starts the sequence; the largest
    # such LF target is row 0's predecessor chain start. Reconstruct each
    # sequence by walking from row 0 backwards via BWT:
    text = bytearray(n)
    j = 0  # row 0 holds the last char of the first sequence in SA order
    # Walk BACKWARD: row i's BWT char is the char preceding the suffix at row i.
    for k in range(n):
        text[(k) % n] = bwt[j]
        j = lf[j]
    # The walk above starts at row 0 (the whole last sequence in reverse);
    # standard reconstruction: t[n-1-k] = bwt[j]; j = LF(j). Redo properly:
    text = bytearray(n)
    j = 0
    for k in range(n):
        text[n - 1 - k] = bwt[j]
        j = lf[j]
    return bytes(text)

def suffix_array(text):
    return sorted(range(len(text)), key=lambda i: text[i:])

def lcp_kasai(text, sa):
    n = len(text)
    rank = [0]*n
    for i, s in enumerate(sa): rank[s] = i
    lcp = [0]*n
    h = 0
    for i in range(n):
        if rank[i] > 0:
            j = sa[rank[i]-1]
            while i+h < n and j+h < n and text[i+h] == text[j+h]:
                h += 1
            lcp[rank[i]] = h
            if h: h -= 1
        else:
            h = 0
    return lcp

def split_runs(syms, lens):
    """Expand grlBWT runs into SPLIT runs (endmarkers one per run)."""
    out = []
    for c, l in zip(syms, lens):
        if c == 0x0A:
            out += [(0x0A, 1)] * l
        else:
            out.append((c, l))
    return out

def brute_thresholds(bwt, lcp):
    """Thresholds over split runs, per TeraLCP semantics (earliest argmin)."""
    runs = []
    i = 0
    n = len(bwt)
    while i < n:
        j = i
        while j < n and bwt[j] == bwt[i]: j += 1
        # split endmarker runs
        if bwt[i] == 0x0A:
            for k in range(i, j): runs.append((0x0A, k, k))
        else:
            runs.append((bwt[i], i, j-1))
        i = j
    thr_out, pos_out = [], []
    last_end = {}
    seen = set()
    for c, s, e in runs:
        if c not in seen:
            thr_out.append(0); pos_out.append(0); seen.add(c)
            last_end[c] = e
            continue
        p = last_end[c]
        if s == p + 1:  # adjacent c-runs
            val, pos = lcp[s], s
        else:
            best, bpos = None, None
            for k in range(p+1, s+1):
                if best is None or lcp[k] < best:
                    best, bpos = lcp[k], k
            val, pos = best, bpos
        thr_out.append(val); pos_out.append(pos)
        last_end[c] = e
    return runs, thr_out, pos_out

def main():
    syms, lens = read_runs(f"{GATE}/t10")
    bwt = expand_bwt(syms, lens)
    n = len(bwt)
    print(f"t10: {len(syms)} runs, n={n}")
    text = invert_bwt(bwt)
    # sanity: re-expand BWT from text via suffix sort and compare
    sa = suffix_array(text)
    bwt2 = bytes(text[(sa[i]-1) % n] for i in range(n))
    assert bwt2 == bwt, "inversion failed: reconstructed text does not reproduce BWT"
    print("inversion verified: text reproduces the BWT exactly")
    lcp = lcp_kasai(text, sa)
    runs, thr, pos = brute_thresholds(bwt, lcp)
    print(f"split runs={len(runs)}")

    # write TeraLCP input (5-byte LE lens) in the same layout grlBWT emits
    prefix = f"{GATE}/t10t"
    with open(f"{prefix}.bwt.heads", "wb") as f:
        f.write(syms)
    with open(f"{prefix}.bwt.len", "wb") as f:
        for l in lens:
            f.write(int(l).to_bytes(5, "little"))
    open(f"{prefix}.scratch", "wb").close()

    # run TeraLCP
    for f in ("thr.thr", "thr.thr_pos", "t10t.lcp_index.lcp_index", "t10t.scratch"):
        try: os.remove(f"{GATE}/{f}")
        except FileNotFoundError: pass
    cmd = [TLC, "-f", "rlbwt", "-i", "t10t", "-t", "t10t.scratch",
           "-oindex", "t10t.lcp_index", "-othresholds", "thr", "--thr-pfp", "-v", "0"]
    r = subprocess.run(cmd, cwd=GATE, capture_output=True, text=True)
    if r.returncode != 0:
        print("TeraLCP FAILED:", r.returncode); print(r.stderr[-2000:]); sys.exit(1)

    got_thr = open(f"{GATE}/thr.thr", "rb").read()
    got_pos = open(f"{GATE}/thr.thr_pos", "rb").read()
    assert len(got_thr) % 5 == 0
    m = len(got_thr)//5
    g_thr = [int.from_bytes(got_thr[i*5:(i+1)*5], "little") for i in range(m)]
    g_pos = [int.from_bytes(got_pos[i*5:(i+1)*5], "little") for i in range(m)]
    print(f"TeraLCP entries={m}, brute entries={len(thr)}")
    if m != len(thr):
        print("ENTRY COUNT MISMATCH"); sys.exit(1)
    bad = [(i, g_thr[i], thr[i], g_pos[i], pos[i]) for i in range(m)
           if g_thr[i] != thr[i] or g_pos[i] != pos[i]]
    if bad:
        print(f"MISMATCHES: {len(bad)} (first 10):")
        for i, gt, bt, gp, bp in bad[:10]:
            print(f"  [{i}] thr tera={gt} brute={bt}  pos tera={gp} brute={bp}")
        sys.exit(1)
    print(f"GATE GREEN: {m}/{m} threshold value+position entries byte-verified")

if __name__ == "__main__":
    main()
