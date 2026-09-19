#!/usr/bin/env python3
"""pilot_patterns.py — sxgc pilot: sample random pattern windows from an AGC
subset served via ragc-ffi + the forward flat sidecar (zero materialization).

Reads the sidecar (cname \t fstart \t len, forward flat offsets), picks random
windows weighted by contig length, extracts each window via ragc-ffi random
access, validates pure-ACGT, and writes:
  <out>            FASTA patterns (forward orientation)
  <out>.truth.tsv  p_name \t forward_flat_pos \t contig \t pattern_len

The truth positions are exact expected occurrences — the pilot gate checks
each appears among the r-index's reported hits (recall) and that every
reported occurrence byte-verifies back through the AGC (precision).
"""
import argparse
import ctypes
import random
import struct
import sys


def load_lib(path):
    lib = ctypes.CDLL(path)
    lib.sxgc_agc_open.restype = ctypes.c_void_p
    lib.sxgc_agc_open.argtypes = [ctypes.c_char_p]
    lib.sxgc_agc_len.restype = ctypes.c_uint64
    lib.sxgc_agc_len.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
    lib.sxgc_agc_range.restype = ctypes.c_int
    lib.sxgc_agc_range.argtypes = [
        ctypes.c_void_p, ctypes.c_char_p,
        ctypes.c_uint64, ctypes.c_uint64,
        ctypes.POINTER(ctypes.c_ubyte),
    ]
    lib.sxgc_agc_close.argtypes = [ctypes.c_void_p]
    lib.sxgc_agc_close.restype = None
    return lib


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--agc", required=True)
    ap.add_argument("--sidecar", required=True)
    ap.add_argument("--lib", default="/home/erikg/sxgc/ragc-ffi/target/release/libragc_ffi.so")
    ap.add_argument("--n", type=int, default=200)
    ap.add_argument("--len", type=int, default=120)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    rows = []
    total = 0
    with open(args.sidecar) as f:
        for line in f:
            cname, fstart, ln = line.rstrip("\n").split("\t")
            fstart, ln = int(fstart), int(ln)
            rows.append((cname, fstart, ln))
            total = max(total, fstart + ln)
    flatlen = total + 1  # trailing '$'
    rows.sort(key=lambda r: r[1])

    lib = load_lib(args.lib)
    h = lib.sxgc_agc_open(args.agc.encode())
    if not h:
        sys.exit("cannot open AGC archive")

    rng = random.Random(args.seed)
    buf = (ctypes.c_ubyte * (args.len + 16))()
    made = 0
    attempts = 0
    with open(args.out, "w") as fa, open(args.out + ".truth.tsv", "w") as truth:
        while made < args.n and attempts < args.n * 200:
            attempts += 1
            o = rng.randrange(0, total)
            # binary search: contig containing flat offset o
            lo, hi = 0, len(rows) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if rows[mid][1] <= o:
                    lo = mid
                else:
                    hi = mid - 1
            cname, fstart, ln = rows[lo]
            if o + args.len >= fstart + ln:  # window must not cross contig end
                continue
            in_off = o - fstart
            got = lib.sxgc_agc_range(h, cname.encode(), in_off, in_off + args.len, buf)
            if got != args.len:
                continue
            seq = bytes(buf[:got])
            if any(c not in b"ACGT" for c in seq):
                continue
            fa.write(f">p{made}\n")
            fa.write(seq.decode() + "\n")
            truth.write(f"p{made}\t{o}\t{cname}\t{args.len}\n")
            made += 1
    lib.sxgc_agc_close(h)
    if made < args.n:
        sys.exit(f"only sampled {made}/{args.n} patterns")
    print(f"patterns: {made} x {args.len} bp; flatlen={flatlen}; wrote {args.out}(+.truth.tsv)")


if __name__ == "__main__":
    main()
