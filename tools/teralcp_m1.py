#!/usr/bin/env python3
"""teralcp_m1.py — M1: does TeraLCP's runtime track the parse/compressed
size, or the text length n?

Method: for each generated text, build the rlbwt DIRECTLY from the brute
suffix array (heads = run BWT char, len = run length; 1 byte + 5 bytes —
the exact format the bit6 gate scripts feed TeraLCP after grlbwt2rle), run
the real TeraLCP binary, and record wall + maxRSS.  Also run pfp++ to get
the parse size.  Then correlate wall time against n, r and parse.

Families:
  F1 random-4, n swept          (r ~ 0.75n, parse small)
  F2 satellites, fixed n, depth swept   (r tiny, parse tiny)
  F3 duplicates, copies swept   (r grows slowly, parse grows with copies)
  F4 fixed-parse + unique filler grown  (parse ~ const, n grows, r grows)

Every number is measured, nothing assumed.  New file; imports gated tools
unmodified.
"""
import os
import resource
import struct
import subprocess
import sys
import time
import random

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from c_probe_common import suffix_array_np  # numpy prefix-doubling SA (gated)

TLC = "/home/erikg/TeraTools/src/TeraLCP/TeraLCP"
PFP = "/home/erikg/pfp/build/pfp++"
WORK = "/tmp/tlc_m1"

# alphabet 0x0b..0x0e (pfp++-safe; order-preserving shift of {A,C,G,T})
ALPH = (0x0B, 0x0C, 0x0D, 0x0E)
A, C, G, TT = 0x0B, 0x0C, 0x0D, 0x0E


def write_rlbwt(name, X):
    sa = suffix_array_np(X)
    N = len(X)
    BWT = [X[sa[i] - 1] if sa[i] else 0 for i in range(N)]
    heads = []
    lens = []
    cur = BWT[0]
    ln = 1
    for i in range(1, N):
        if BWT[i] == cur:
            ln += 1
        else:
            heads.append(cur)
            lens.append(ln)
            cur = BWT[i]
            ln = 1
    heads.append(cur)
    lens.append(ln)
    base = os.path.join(WORK, name)
    open(base + ".bwt.heads", "wb").write(bytes(heads))
    with open(base + ".bwt.len", "wb") as f:
        for v in lens:
            f.write(v.to_bytes(5, "little"))
    open(base + ".scratch", "wb").close()
    return len(heads), N


def run_teralcp(name):
    base = os.path.join(WORK, name)
    r0 = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    t0 = time.perf_counter()
    p = subprocess.run(
        [TLC, "-f", "rlbwt", "-i", base, "-t", base + ".scratch",
         "-oindex", base + ".lcp_index", "-v", "quiet"],
        capture_output=True, text=True)
    dt = time.perf_counter() - t0
    r1 = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    ok = p.returncode == 0 and os.path.exists(base + ".lcp_index.lcp_index")
    return dt, max(r1, r0) / 1024.0, ok, p.stderr.strip().splitlines()[-1:] if p.stderr else []


def run_pfp(name, X):
    base = os.path.join(WORK, name + "_pfp")
    open(base + ".txt", "wb").write(X)
    p = subprocess.run([PFP, "-t", base + ".txt", "-o", base, "-w", "10", "-p", "100"],
                       capture_output=True, text=True)
    plen = None
    if os.path.exists(base + ".parse"):
        plen = os.path.getsize(base + ".parse") // 4
    # dictionary size (distinct phrases): count records in .dict (rough: file size / entry)
    dsize = os.path.getsize(base + ".dict") if os.path.exists(base + ".dict") else None
    return plen, dsize


def measure(label, T, do_pfp=True):
    name = label.replace("+", "_").replace(".", "_")
    X = T + b"\x00"
    R, N = write_rlbwt(name, X)
    dt, rss, ok, err = run_teralcp(name)
    plen = dsize = None
    if do_pfp:
        try:
            plen, dsize = run_pfp(name, T)
        except Exception as e:  # noqa
            pass
    print(f"{label:28s} n={N:8d} r={R:8d} parse={str(plen):>9s} dictB={str(dsize):>10s} "
          f"TLC={dt*1000:9.1f}ms RSS={rss:8.1f}MB {'OK' if ok else 'FAIL ' + str(err)}",
          flush=True)
    return dict(label=label, n=N, r=R, parse=plen, dict=dsize, tlc=dt, rss=rss, ok=ok)


def f1_random(sizes=(50_000, 100_000, 200_000, 400_000, 800_000)):
    rng = random.Random(101)
    out = []
    for n in sizes:
        txt = bytes(rng.choice(ALPH) for _ in range(n))
        out.append((f"F1 random-4-{n}", txt))
    return out


def f2_satellite(n=200_000, depths=(1, 2, 3, 4)):
    rng = random.Random(202)
    out = []
    for d in depths:
        sat = bytes((A, C, G))
        for _ in range(d - 1):
            sat = (sat * max(2, 2000 // len(sat))) + bytes((TT,))
        reps = max(1, n // len(sat))
        txt = sat * reps + bytes(rng.choice(ALPH) for _ in range(400))
        out.append((f"F2 sat-depth{d}", txt))
    return out


def f3_duplicates(copies=(5, 10, 50, 200), nparas=40, plen=300):
    rng = random.Random(303)
    paras = [bytes(rng.choice(ALPH) for _ in range(plen)) for _ in range(nparas)]
    out = []
    for c in copies:
        docs = []
        for _ in range(c):
            for p in rng.sample(paras, len(paras)):
                docs.append(p)
        out.append((f"F3 dup-copies{c}", b"".join(docs)))
    return out


def f4_fixed_parse(fillers=(0, 50_000, 200_000, 800_000), copies=20, nparas=40, plen=300):
    rng = random.Random(404)
    paras = [bytes(rng.choice(ALPH) for _ in range(plen)) for _ in range(nparas)]
    docs = []
    for _ in range(copies):
        for p in rng.sample(paras, len(paras)):
            docs.append(p)
    base = b"".join(docs)
    out = []
    for extra in fillers:
        txt = base + bytes(rng.choice(ALPH) for _ in range(extra))
        out.append((f"F4 fixparse+{extra}", txt))
    return out


def main():
    os.makedirs(WORK, exist_ok=True)
    only = sys.argv[1:] if len(sys.argv) > 1 else None
    rows = []

    def run_family(tag, items):
        if only and tag not in only:
            return
        for label, T in items:
            rows.append(measure(label, T))

    run_family("F1", f1_random())
    run_family("F2", f2_satellite())
    run_family("F3", f3_duplicates())
    run_family("F4", f4_fixed_parse())

    print("\n=== per-family slopes (tlc vs n, vs r, vs parse) ===")
    import math
    for tag in ("F1", "F2", "F3", "F4"):
        fam = [r for r in rows if r["label"].startswith(tag) and r["ok"]]
        if len(fam) < 2:
            continue
        print(f"{tag}: " + "  ".join(f"{r['label'].split()[1]}: n={r['n']} r={r['r']} "
                                    f"parse={r['parse']} t={r['tlc']*1000:.1f}ms" for r in fam))
    print("M1 DONE")


if __name__ == "__main__":
    main()
