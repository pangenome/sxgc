#!/usr/bin/env python3
"""pilot_verify.py — sxgc pilot gate: verify r-index query output against the
AGC-served forward text (zero materialization).

For every reported occurrence (pos, mlen): map pos through the sidecar to
(cname, in-contig offset), extract mlen bytes via ragc-ffi, byte-compare with
the pattern (precision). Also check every planted truth position appears
among the reported occurrences (recall). Emits a single VERDICT line.
"""
import argparse
import bisect
import ctypes
import sys


def load_lib(path):
    lib = ctypes.CDLL(path)
    lib.sxgc_agc_open.restype = ctypes.c_void_p
    lib.sxgc_agc_open.argtypes = [ctypes.c_char_p]
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
    ap.add_argument("--patterns", required=True)
    ap.add_argument("--truth", required=True)
    ap.add_argument("--occs", required=True)
    ap.add_argument("--lib", default="/home/erikg/sxgc/ragc-ffi/target/release/libragc_ffi.so")
    ap.add_argument("--maxlen", type=int, default=512)
    args = ap.parse_args()

    # sidecar, sorted by forward offset, with binary search index
    rows = []
    with open(args.sidecar) as f:
        for line in f:
            cname, fstart, ln = line.rstrip("\n").split("\t")
            rows.append((cname, int(fstart), int(ln)))
    rows.sort(key=lambda r: r[1])   # bisect needs offset-sorted rows
    meta = rows
    starts = [r[1] for r in rows]

    # patterns
    pats, cur = {}, None
    with open(args.patterns) as f:
        for line in f:
            line = line.strip()
            if line.startswith(">"):
                cur = line[1:]
                pats[cur] = ""
            elif cur:
                pats[cur] += line

    # truth: p -> expected forward position
    truth = {}
    with open(args.truth) as f:
        for line in f:
            name, pos, cname, ln = line.split("\t")
            truth[name] = int(pos)

    # occurrences
    occs, cur = {}, None
    with open(args.occs) as f:
        for line in f:
            line = line.strip()
            if line.startswith(">"):
                cur = line[1:]
            elif cur is not None:
                pos, mlen = line.split()
                occs.setdefault(cur, []).append((int(pos), int(mlen)))

    lib = load_lib(args.lib)
    h = lib.sxgc_agc_open(args.agc.encode())
    if not h:
        sys.exit("cannot open AGC archive")
    maxml = max((max((m for _, m in v), default=0) for v in occs.values()), default=64)
    buf = (ctypes.c_ubyte * (max(maxml, 64) + 16))()

    n_occ = n_ok = n_bad = 0
    missing = []
    for name, pat in pats.items():
        got = occs.get(name, [])
        exp_pos = truth.get(name)
        if exp_pos is None or not any(p == exp_pos and m == len(pat) for p, m in got):
            missing.append(name)
        pb = pat.encode()
        for pos, mlen in got:
            n_occ += 1
            if mlen != len(pat):
                n_bad += 1
                continue
            i = bisect.bisect_right(starts, pos) - 1
            if i < 0:
                n_bad += 1
                continue
            cname, fstart, ln = meta[i]
            if pos < fstart or pos + mlen > fstart + ln:  # crossing '$' — cannot match pure DNA
                n_bad += 1
                continue
            in_off = pos - fstart
            if lib.sxgc_agc_range(h, cname.encode(), in_off, in_off + mlen, buf) != mlen:
                n_bad += 1
                continue
            if bytes(buf[:mlen]) == pb:
                n_ok += 1
            else:
                n_bad += 1
    lib.sxgc_agc_close(h)

    print(f"occurrences: {n_occ}, byte-verified: {n_ok}, bad: {n_bad}")
    print(f"planted truths found: {len(pats) - len(missing)}/{len(pats)}" +
          (f" MISSING: {missing[:8]}" if missing else ""))
    if n_bad == 0 and not missing and n_occ > 0:
        print("PILOT VERDICT GREEN")
        return 0
    print("PILOT VERDICT FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
