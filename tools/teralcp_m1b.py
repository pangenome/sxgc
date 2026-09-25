#!/usr/bin/env python3
"""teralcp_m1b.py — M1 at scale: TeraLCP time/space vs n at FIXED tiny r
(satellite) vs FIXED growing r (random), plus the startup floor.

The decisive comparison for "parse time vs n":
  random-4 n=3.2M..25.6M  -> r ~ 0.75n  (both grow)
  satellite n=3.2M..25.6M -> r ~ 300    (n grows, r fixed)
If satellite times stay ~flat while random times grow -> TeraLCP is
r-driven at scale.  If both grow alike -> n-driven.

Threads fixed (-p 1) for a clean law; one default-thread control run.
"""
import os
import subprocess
import sys
import time
import random
import resource

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from c_probe_common import suffix_array_np

TLC = "/home/erikg/TeraTools/src/TeraLCP/TeraLCP"
WORK = "/tmp/tlc_m1b"
A, C, G, TT = 0x0B, 0x0C, 0x0D, 0x0E
ALPH = (A, C, G, TT)


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


def run_teralcp(name, threads=1):
    base = os.path.join(WORK, name)
    args = [TLC, "-f", "rlbwt", "-i", base, "-t", base + ".scratch",
            "-oindex", base + ".lcp_index", "-v", "quiet", "-p", str(threads)]
    t0 = time.perf_counter()
    p = subprocess.run(args, capture_output=True, text=True)
    dt = time.perf_counter() - t0
    idx = base + ".lcp_index.lcp_index"
    isz = os.path.getsize(idx) if os.path.exists(idx) else 0
    ok = p.returncode == 0 and isz > 0
    return dt, isz, ok


def measure(label, T, threads=1, do_pfp=False):
    name = label.replace(" ", "_").replace("+", "_").replace(".", "_")
    X = T + b"\x00"
    t0 = time.perf_counter()
    R, N = write_rlbwt(name, X)
    t_sa = time.perf_counter() - t0
    # warm re-run x2, report min
    times = []
    for _ in range(2):
        dt, isz, ok = run_teralcp(name, threads)
        times.append(dt)
    dt = min(times)
    plen = ""
    if do_pfp:
        base = os.path.join(WORK, name + "_pfp")
        open(base + ".txt", "wb").write(T)
        subprocess.run(["/home/erikg/pfp/build/pfp++", "-t", base + ".txt", "-o", base,
                        "-w", "10", "-p", "100"], capture_output=True, text=True)
        if os.path.exists(base + ".parse"):
            plen = os.path.getsize(base + ".parse") // 4
    print(f"{label:24s} n={N:10d} r={R:9d} parse={str(plen):>8s} idx={isz/1e6:8.1f}MB "
          f"TLC={dt*1000:9.1f}ms (SA-build {t_sa:.1f}s) {'OK' if ok else 'FAIL'}", flush=True)
    return dict(label=label, n=N, r=R, parse=plen, idx=isz, t=dt, ok=ok)


def main():
    os.makedirs(WORK, exist_ok=True)
    only = sys.argv[1] if len(sys.argv) > 1 else None
    rng = random.Random(77)
    rows = []

    # startup floor
    if not only or "floor" in only:
        rows.append(measure("floor-n1000", bytes(rng.choice(ALPH) for _ in range(1000))))

    sizes = (3_200_000, 6_400_000, 12_800_000, 25_600_000)
    if not only or "F1" in only:
        for n in sizes:
            T = bytes(rng.choice(ALPH) for _ in range(n))
            rows.append(measure(f"F1 random-4-{n//1000}k", T))
    if not only or "F2" in only:
        for n in sizes:
            sat = b"\x0b\x0c\x0d" * (n // 3)
            T = sat + bytes(rng.choice(ALPH) for _ in range(400))
            rows.append(measure(f"F2 sat-{n//1000}k", T))
    if not only or "F3" in only:
        paras = [bytes(rng.choice(ALPH) for _ in range(300)) for _ in range(40)]
        for copies in (400, 800, 1600):
            docs = []
            for _ in range(copies):
                for p in rng.sample(paras, len(paras)):
                    docs.append(p)
            T = b"".join(docs)
            rows.append(measure(f"F3 dup-{copies}x", T))
    # thread control on the biggest random
    if not only or "threads" in only:
        n = 12_800_000
        T = bytes(rng.choice(ALPH) for _ in range(n))
        name = "ctl-rand-12_8M"
        X = T + b"\x00"
        R, N = write_rlbwt(name, X)
        for th in (1, 4, 32):
            times = [run_teralcp(name, th)[0] for _ in range(2)]
            print(f"threads={th:3d}  n={N} r={R} TLC={min(times)*1000:.1f}ms", flush=True)

    print("\n=== SCALING SUMMARY (TeraLCP wall, -p 1) ===")
    for pre in ("floor", "F1", "F2", "F3"):
        fam = [r for r in rows if r["label"].startswith(pre)]
        for r in fam:
            print(f"  {r['label']:22s} n={r['n']:10d} r={r['r']:9d} t={r['t']*1000:9.1f}ms "
                  f"t/n={r['t']/r['n']*1e9:7.2f}ns/bp")
    print("M1B DONE")


if __name__ == "__main__":
    main()
