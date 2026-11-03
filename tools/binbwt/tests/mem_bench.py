#!/usr/bin/env python3
"""X4 throughput gate for `xsa mem --serve` (framed maximal-exact-match mode).

Sends N short queries as request frames, in order, on one open, and reports
queries/s and microseconds per query for the timed region (after a warmup).
Every reply must be a 16-byte payload; the queries at known indices that do not
occur must come back with best_len 0, which is how request ORDER is verified
without an id field in the frame.

Usage: mem_bench.py XSA CONTAINER CORPUS --out DIR [-j 32] [-n 10000] [--warmup 200]
                    [--query-len 32] [--seed 20261104]
"""
import argparse
import json
import mmap
import os
import random
import struct
import subprocess
import sys
import time


def die(m):
    sys.stderr.write("mem_bench: FAIL: %s\n" % m)
    sys.exit(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xsa")
    ap.add_argument("container")
    ap.add_argument("corpus")
    ap.add_argument("--out", required=True)
    ap.add_argument("-j", type=int, default=32)
    ap.add_argument("-n", type=int, default=10000)
    ap.add_argument("--warmup", type=int, default=200)
    ap.add_argument("--query-len", type=int, default=32)
    ap.add_argument("--seed", type=int, default=20261104)
    ap.add_argument("--min-len", type=int, default=8)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    rng = random.Random(a.seed)
    n = os.path.getsize(a.corpus)
    f = open(a.corpus, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    qs = []
    while len(qs) < a.n:
        pos = rng.randrange(0, n - a.query_len - 1)
        qs.append(bytes(mm[pos:pos + a.query_len]))
    mm.close()
    f.close()
    # Absent markers: byte strings that cannot occur (a run of 0xFF bytes is not
    # in this text corpus; verified by the answer, not assumed).
    absent_idx = list(range(0, a.n, max(1, a.n // 16)))
    for i in absent_idx:
        qs[i] = b"\xff" * a.query_len
    cmd = [a.xsa, "mem", a.container, "--serve", "-j", str(a.j), "--min-len",
           str(a.min_len)]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    warm = qs[:a.warmup]
    body = b"".join(struct.pack("<I", len(q)) + q for q in warm)
    t_warm = time.time()
    p.stdin.write(body)
    p.stdin.flush()
    got = 0
    warm_out = []
    while got < len(warm):
        hdr = p.stdout.read(4)
        if len(hdr) < 4:
            die("short read during warmup")
        ln = struct.unpack("<I", hdr)[0]
        warm_out.append(p.stdout.read(ln))
        got += 1
    warm_s = time.time() - t_warm
    timed = qs[a.warmup:]
    body = b"".join(struct.pack("<I", len(q)) + q for q in timed)
    t0 = time.time()
    p.stdin.write(body)
    p.stdin.flush()
    replies = []
    while len(replies) < len(timed):
        hdr = p.stdout.read(4)
        if len(hdr) < 4:
            die("short read: %d/%d replies" % (len(replies), len(timed)))
        ln = struct.unpack("<I", hdr)[0]
        b = p.stdout.read(ln)
        if len(b) != ln:
            die("short payload")
        if ln != 16:
            die("payload %d != 16" % ln)
        replies.append(struct.unpack("<IIQ", b))
    secs = time.time() - t0
    p.stdin.close()
    err = p.stderr.read().decode()
    p.wait()
    if p.returncode != 0:
        die("serve exited %d: %s" % (p.returncode, err[-2000:]))
    sys.stderr.write(err)
    # Order check: the absent markers must come back as best_len 0.
    for i in absent_idx:
        j = i - a.warmup
        if j < 0:
            continue
        if replies[j][1] != 0:
            die("frame %d should be absent (best_len 0), got %s" % (i, replies[j]))
    # Attested answers: every returned match must be right-maximal by the frame
    # contract, i.e. its length may not exceed the query length.
    for (qs_start, blen, occ) in replies:
        if blen > a.query_len or qs_start + blen > a.query_len:
            die("impossible frame start=%d len=%d" % (qs_start, blen))
        if occ > n:
            die("occurrences %d > corpus %d" % (occ, n))
    res = {
        "workers": a.j,
        "queries": len(timed),
        "warmup": len(warm),
        "query_len": a.query_len,
        "seconds": secs,
        "queries_per_s": len(timed) / secs,
        "us_per_query": secs * 1e6 / len(timed),
        "warmup_seconds": warm_s,
        "container": a.container,
        "corpus": a.corpus,
        "stderr": err.strip().splitlines(),
    }
    path = os.path.join(a.out, "mem_bench.json")
    with open(path, "w") as fh:
        json.dump(res, fh, indent=1)
    print("[X4] queries=%d workers=%d seconds=%.3f queries_per_s=%.3f us_per_query=%.1f" %
          (len(timed), a.j, secs, res["queries_per_s"], res["us_per_query"]))
    print("[X4] wrote %s" % path)


if __name__ == "__main__":
    main()
