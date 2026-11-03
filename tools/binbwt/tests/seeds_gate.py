#!/usr/bin/env python3
"""xsa seeds gate: the length solve, the band, containment, guards, determinism.

The oracle is tests/seeds_oracle.c: one mmap pass per pattern with glibc memmem,
OVERLAPPING occurrences (what a BWT interval width means), no index code and no
shared implementation with xsa.

    gcc -O2 -o seeds_oracle tools/binbwt/tests/seeds_oracle.c
    python3 tools/binbwt/tests/seeds_gate.py XSA CONTAINER CORPUS --out DIR

Gates:
  S1  mechanism - for every emitted seed the true corpus multiplicity equals the
      reported `occurrences`; the achieved multiplicity reaches the target; the
      seed one byte longer falls BELOW the target (the length the solve gave up
      at); multiplicity is non-increasing over EVERY length of a probe ladder at
      each emitted offset (the monotonicity the bisection assumes); no emitted
      seed is contained in another and no seed content repeats; the band ceiling
      is respected in band mode; the window counters add up exactly.
  S3  guards - --max-len below --min-len refused; empty text refused; a text
      with no matches yields zero seeds cleanly; 0x1E is an ordinary byte (a
      slice spanning a document separator seeds and the oracle agrees); a
      zero target and two simultaneous multiplicity modes refused.
  S4  determinism - the same input twice gives byte-identical output.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)

def counts(oracle, corpus, pats, scratch):
    """True overlapping occurrence counts for a list of byte strings."""
    path = os.path.join(scratch, "patterns.bin")
    with open(path, "wb") as f:
        for p in pats:
            f.write(struct.pack("<I", len(p)))
            f.write(p)
    r = sh([oracle, corpus, path])
    if r.returncode != 0:
        sys.exit(f"oracle failed: {r.stderr.decode()[:400]}")
    out = [int(x) for x in r.stdout.split()]
    if len(out) != len(pats):
        sys.exit(f"oracle returned {len(out)} counts for {len(pats)} patterns")
    return out

def run_seeds(xsa, container, text, out, extra):
    r = sh([xsa, "seeds", container, text] + extra)
    if r.returncode != 0:
        sys.exit(f"xsa seeds failed ({r.returncode}): {r.stderr.decode()[:800]}")
    return r.stdout, r.stderr.decode()

def seeds_of(stderr, path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]

def stats_of(stderr):
    for line in stderr.splitlines():
        if line.startswith("SEEDS_STATS "):
            return dict(kv.split("=", 1) for kv in line[len("SEEDS_STATS "):].split())
    sys.exit("no SEEDS_STATS line")

def strip_timings(s):
    return {k: v for k, v in s.items() if not k.endswith("_ms")}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xsa")
    ap.add_argument("container")
    ap.add_argument("corpus")
    ap.add_argument("--out", required=True)
    ap.add_argument("--docs")
    args = ap.parse_args()
    out = args.out
    os.makedirs(out, exist_ok=True)
    oracle = os.path.join(out, "seeds_oracle")
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seeds_oracle.c")
    r = sh(["gcc", "-O2", "-o", oracle, src])
    if r.returncode != 0:
        sys.exit(f"oracle build failed: {r.stderr.decode()[:400]}")

    # ---- fixture texts. A 1200-byte slice of the corpus (so lengths walk a real
    # multiplicity ladder), plus synthetic lines that are absent or rare.
    with open(args.corpus, "rb") as f:
        f.seek(1234567)
        slice_ = f.read(1200)
    with open(args.corpus, "rb") as f:
        f.seek(4096)
        window = f.read(1 << 16)
    sep = window.find(b"\x1e")
    if sep < 0:
        sys.exit("corpus has no 0x1E in the first 64 KiB after 4096: pick another slice")
    with open(args.corpus, "rb") as f:
        f.seek(4096 + sep - 6)
        sentinel_slice = f.read(64)
    assert b"\x1e" in sentinel_slice

    mixed = os.path.join(out, "text_mixed.bin")
    with open(mixed, "wb") as f:
        f.write(slice_ + b"\nTHE-TWICE-TEST-phrase-9f3a\nSENTINEL\x1eHANDLING\x1eBYTES\n")
    sent = os.path.join(out, "text_sentinel.bin")
    with open(sent, "wb") as f:
        f.write(sentinel_slice)
    absent = os.path.join(out, "text_absent.bin")
    # Bytes 0x00..0x03 do not occur in this corpus at all (checked: exact count 0),
    # so every window is a NoMatch - the clean zero-seed case.
    with open(absent, "wb") as f:
        f.write(bytes([0, 1, 2, 3]) * 16)
    empty = os.path.join(out, "text_empty.bin")
    open(empty, "wb").close()

    T = 50
    bad = 0
    def check(cond, msg):
        nonlocal bad
        if not cond:
            bad += 1
            print(f"  FAIL {msg}")
        return cond

    # ---------------------------------------------------------------- S1
    print(f"S1 mechanism: target-mult {T}, text 1200 bytes of corpus + synthetic lines")
    j1 = os.path.join(out, "s1_target.jsonl")
    out1, err1 = run_seeds(args.xsa, args.container, mixed, out,
                           ["--target-mult", str(T), "--max-len", "64", "--min-len", "12",
                            "--max-seeds", "25", "--stride", "32", "--jsonl",
                            "--limit", "4"] + (["--docs", args.docs] if args.docs else []))
    with open(j1, "wb") as f:
        f.write(out1)
    s1 = seeds_of(err1, j1)
    st1 = stats_of(err1)
    print(f"  seeds={len(s1)} stats={ {k: v for k, v in st1.items() if k in ('windows','tried','accepted','mean_len','mult_histogram','no_match','too_short','too_rare','too_common')} }")
    check(len(s1) > 0, "S1 emitted no seed at all")
    with open(mixed, "rb") as f:
        text = f.read()
    pats, meta = [], []
    for s in s1:
        off, ln = s["text_offset"], s["length"]
        pats.append(text[off:off + ln])
        meta.append((s, text[off:off + ln + 1]))
    tcounts = counts(oracle, args.corpus, pats, out)
    for (s, plus1), c in zip(meta, tcounts):
        off, ln = s["text_offset"], s["length"]
        check(c == s["occurrences"],
              f"S1 offset {off} len {ln}: oracle {c} != reported {s['occurrences']}")
        check(c >= T, f"S1 offset {off} len {ln}: multiplicity {c} did not reach target {T}")
        if s["gave_up_len"] is not None:
            check(s["gave_up_len"] == ln + 1,
                  f"S1 offset {off}: gave_up_len {s['gave_up_len']} is not len+1")
            g = counts(oracle, args.corpus, [text[off:off + s["gave_up_len"]]], out)[0]
            check(g == s["gave_up_mult"],
                  f"S1 offset {off}: gave_up_mult reported {s['gave_up_mult']}, oracle {g}")
            check(g < T, f"S1 offset {off}: the one-byte-longer seed has multiplicity {g} >= {T}")
    # the ladder: multiplicity must be non-increasing over EVERY length at the
    # offset, i.e. the monotonicity the bisection relies on.
    print("  length ladder (true counts) at the first 4 emitted offsets:")
    for s in s1[:4]:
        off = s["text_offset"]
        lens = list(range(12, 33))
        ladder = counts(oracle, args.corpus, [text[off:off + m] for m in lens], out)
        print(f"    offset {off}: " + " ".join(f"{m}:{c}" for m, c in zip(lens, ladder)))
        for a, b in zip(ladder, ladder[1:]):
            if not check(a >= b, f"S1 offset {off}: multiplicity rose with length ({a} -> {b})"):
                break
        base = lens[0]
        while base > 1 and counts(oracle, args.corpus, [text[off:off + base]], out)[0] < T:
            base -= 1
        if base >= 12:
            c_at = counts(oracle, args.corpus, [text[off:off + base]], out)[0]
            check(c_at >= T, f"S1 offset {off}: the reported length {s['length']} is shorter than "
                             f"the longest length {base} still reaching the target")
    # containment and dedup
    ivs = [(s["text_offset"], s["text_offset"] + s["length"]) for s in s1]
    for i, (a, b) in enumerate(ivs):
        for j, (c, d) in enumerate(ivs):
            if i != j:
                check(not (c <= a and b <= d), f"S1 seed {i} {a}..{b} is contained in seed {j} {c}..{d}")
    bodies = [text[a:b] for a, b in ivs]
    check(len(set(bodies)) == len(bodies), "S1 duplicate seed content was emitted")
    check(int(st1["accepted"]) == len(s1), f"S1 accepted {st1['accepted']} != {len(s1)} emitted")
    tot = sum(int(st1[k]) for k in ("no_match", "too_short", "too_rare", "too_common"))
    check(int(st1["windows"]) == tot + len(s1) + int(st1["suppressed_dedup"])
          + int(st1["suppressed_contained"]),
          f"S1 window counters do not add up: {st1}")

    # band mode: the cap is respected and the band is the solve target
    jb = os.path.join(out, "s1_band.jsonl")
    resid = sh([args.xsa, "seeds", args.container, mixed, "--max-mult", "80", "--min-mult", "50",
                "--max-len", "64", "--min-len", "12", "--max-seeds", "25", "--stride", "32",
                "--jsonl", "--limit", "4"] + (["--docs", args.docs] if args.docs else []))
    if resid.returncode != 0:
        sys.exit(f"band mode failed: {resid.stderr.decode()[:400]}")
    with open(jb, "w") as f:
        f.write(resid.stdout.decode())
    sb = seeds_of(resid.stderr.decode(), jb)
    stb = stats_of(resid.stderr.decode())
    print(f"  band 50..80: seeds={len(sb)} too_common={stb['too_common']} too_rare={stb['too_rare']}")
    for s in sb:
        check(50 <= s["occurrences"] <= 80,
              f"S1 band: offset {s['text_offset']} multiplicity {s['occurrences']} outside 50..80")

    # a window whose longest occurring prefix is still above the cap is too_common
    # A window whose LONGEST occurring prefix is still far above the target: 12
    # runs of 'A' occur hundreds of times in this corpus, so with --max-len 12
    # there is no length at or below the target to solve for.
    common = os.path.join(out, "text_common.bin")
    with open(common, "wb") as f:
        f.write(b"A" * 96)
    rc = sh([args.xsa, "seeds", args.container, common, "--target-mult", "2", "--max-len", "12",
             "--min-len", "12", "--max-seeds", "5", "--stride", "6", "--jsonl",
             "--limit", "0"] + (["--docs", args.docs] if args.docs else []))
    if rc.returncode != 0:
        sys.exit(f"too_common case failed: {rc.stderr.decode()[:400]}")
    stc = stats_of(rc.stderr.decode())
    print(f"  too_common case: windows={stc['windows']} too_common={stc['too_common']} "
          f"too_rare={stc['too_rare']} no_match={stc['no_match']} "
          f"emitted_lines={len(rc.stdout.splitlines())}")
    check(int(stc["too_common"]) > 0, "S1 a window far above the target was not counted too_common")
    check(rc.stdout.strip() == b"", "S1 too_common window still emitted a seed")

    # ---------------------------------------------------------------- S3
    print("S3 guards")
    g = sh([args.xsa, "seeds", args.container, mixed, "--max-len", "8", "--min-len", "12"])
    check(g.returncode != 0 and b"smaller than --min-len" in g.stderr,
          f"S3 --max-len below --min-len was not refused: {g.returncode} {g.stderr[:200]}")
    g = sh([args.xsa, "seeds", args.container, empty])
    check(g.returncode != 0 and b"empty text" in g.stderr,
          f"S3 empty text was not refused: {g.returncode} {g.stderr[:200]}")
    g = sh([args.xsa, "seeds", args.container, absent, "--target-mult", "5", "--jsonl"])
    check(g.returncode == 0, f"S3 absent text failed instead of reporting zero seeds: {g.stderr[:200]}")
    check(g.stdout.strip() == b"", "S3 a text with no matches emitted rows")
    sta = stats_of(g.stderr.decode())
    check(int(sta["accepted"]) == 0, "S3 absent text accepted a seed")
    print(f"  absent text: windows={sta['windows']} no_match={sta['no_match']} too_rare={sta['too_rare']}")
    # 0x1E is an ordinary byte: a slice spanning a document separator still seeds,
    # and the oracle (which has no notion of separators) agrees on the counts.
    js = os.path.join(out, "s3_sentinel.jsonl")
    # target 1 = "the longest occurring prefix, whatever its multiplicity": the
    # multiplicity-1 path, which is also what proves 0x1E is not special.
    rs = sh([args.xsa, "seeds", args.container, sent, "--target-mult", "1", "--max-len", "40",
             "--min-len", "12", "--max-seeds", "12", "--stride", "8", "--jsonl", "--limit", "4"])
    check(rs.returncode == 0, f"S3 sentinel text failed: {rs.stderr.decode()[:300]}")
    with open(js, "w") as f:
        f.write(rs.stdout.decode())
    ss = seeds_of(rs.stderr.decode(), js)
    with open(sent, "rb") as f:
        sent_text = f.read()
    n_sep = sent_text.count(bytes([0x1E]))
    print(f"  sentinel slice: {len(sent_text)} bytes, {n_sep} 0x1E bytes, {len(ss)} seeds")
    check(len(ss) > 0, "S3 a slice spanning 0x1E produced no seeds")
    cc = counts(oracle, args.corpus, [sent_text[s["text_offset"]:s["text_offset"] + s["length"]] for s in ss], out)
    for s, c in zip(ss, cc):
        check(c == s["occurrences"], f"S3 sentinel seed at {s['text_offset']}: oracle {c} != {s['occurrences']}")
    SEP = bytes([0x1E])
    check(any(SEP in sent_text[s["text_offset"]:s["text_offset"] + s["length"]] for s in ss),
          "S3 no emitted seed actually spans the 0x1E separator")
    g = sh([args.xsa, "seeds", args.container, mixed, "--target-mult", "0"])
    check(g.returncode != 0 and b"must be positive" in g.stderr, "S3 a zero target was not refused")
    g = sh([args.xsa, "seeds", args.container, mixed, "--target-mult", "5", "--max-mult", "10"])
    check(g.returncode != 0 and b"alternative modes" in g.stderr,
          "S3 --target-mult with a band was not refused")
    g = sh([args.xsa, "seeds", args.container, mixed, "--max-mult", "10"])
    check(g.returncode != 0 and b"both --min-mult and --max-mult" in g.stderr,
          "S3 a half band was not refused")

    # ---------------------------------------------------------------- S4
    print("S4 determinism")
    outs = []
    for _ in range(2):
        r = sh([args.xsa, "seeds", args.container, mixed, "--target-mult", "50", "--max-len", "64",
                "--min-len", "12", "--max-seeds", "25", "--stride", "32", "--jsonl", "--limit", "4"]
               + (["--docs", args.docs] if args.docs else []))
        outs.append((r.stdout, strip_timings(stats_of(r.stderr.decode()))))
    check(outs[0][0] == outs[1][0], "S4 two runs differ in their seed output")
    check(outs[0][1] == outs[1][1], f"S4 two runs differ in their counters: {outs[0][1]} vs {outs[1][1]}")
    check(outs[0][0] == open(j1, "rb").read(), "S4 rerun differs from the S1 output")

    print("RESULT", "PASS" if bad == 0 else f"FAIL ({bad} checks)", f"out={out}")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main())
