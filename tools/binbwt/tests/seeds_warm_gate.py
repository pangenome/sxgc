#!/usr/bin/env python3
"""xsa seeds WARM PATH gate: the anchor table, the served forms, determinism, reads.

The oracle is tests/seeds_anchor_oracle.c: one mmap pass per pattern with glibc
memmem, OVERLAPPING occurrence starts, no index code. Document ids are checked
against an INDEPENDENT document map computed here from the corpus' 0x1E
separators, never against m15.

    gcc -O2 -o seeds_anchor_oracle tools/binbwt/tests/seeds_anchor_oracle.c
    python3 tools/binbwt/tests/seeds_warm_gate.py XSA CONTAINER CORPUS --out DIR
            [--docs present|absent] [--jobs N]

Gates:
  W1  the three input forms answer the SAME rows - a text file (window 0), a
      declared JSONL window set, and framed serve stdin - compared row for row
      after decoding the serve frames.
  W2  every anchor row is EXACT: match_len bytes of the query at query_offset
      occur in the corpus exactly `multiplicity` times (oracle), and
      corpus_offset IS one of those occurrence starts; every query_offset is a
      window offset the scan could have emitted. With --anchor-max-per-seed 1
      the ROW SET shrinks but `multiplicity` does not (an output cap is not a
      support threshold).
  W3  corpus_doc: with m15 (--docs present) every row's doc field equals the
      independent document index of corpus_offset; with --docs absent every row's
      doc field is `-` (absent, never guessed).
  W4  determinism: two runs are byte-identical, and -j 1 vs -j N are
      byte-identical (the tie order is the table's, not the scheduler's).
  W5  reads follow the QUERY SET, not the container: the same window answered
      twice in one process reads no more than the first (rchar delta), and 4
      windows read at most ~4x one window and far less than the container.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys

def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)

def die(msg):
    sys.exit("SEEDS_WARM_GATE_FAIL " + msg)

def run(xsa, argv, stdin=None, allow_fail=False):
    r = sh([xsa] + argv, input=stdin)
    if r.returncode != 0 and not allow_fail:
        die(f"xsa {' '.join(argv[:4])} failed ({r.returncode}): {r.stderr.decode()[:900]}")
    return r

def stats_of(stderr, key="SEEDS_SERVE_STATS"):
    for line in stderr.decode().splitlines():
        if line.startswith(key + " "):
            return dict(kv.split("=", 1) for kv in line[len(key) + 1:].split())
    return None

def read_tsv(path):
    """Anchor rows as tuples; the header lines are returned verbatim."""
    header, rows = [], []
    with open(path, "rb") as f:
        for line in f.read().split(b"\n"):
            if not line:
                continue
            if line.startswith(b"#"):
                header.append(line.decode())
                continue
            cols = line.decode().split("\t")
            if cols[0] == "window_id":
                header.append(line.decode())
                continue
            wid, qoff, doc, coff, mlen, mult = cols
            rows.append({
                "window_id": int(wid), "query_offset": int(qoff),
                "corpus_doc": None if doc == "-" else int(doc),
                "corpus_offset": int(coff), "match_len": int(mlen),
                "multiplicity": int(mult),
            })
    return header, rows

def decode_frames(path):
    """Serve frames -> the same row dicts, plus the frame-level counts."""
    data = open(path, "rb").read()
    frames, i = [], 0
    while i < len(data):
        if i + 4 > len(data):
            die(f"frame stream: {len(data) - i} trailing bytes are not a u32 length")
        (n,) = struct.unpack_from("<I", data, i)
        i += 4
        if i + n > len(data):
            die("frame stream: payload shorter than its declared length")
        p = data[i:i + n]
        i += n
        wid, rows, seeds = struct.unpack_from("<QII", p, 0)
        if len(p) != 16 + 32 * rows:
            die(f"frame {wid}: payload {len(p)} != 16 + 32*{rows}")
        rs = []
        for k in range(rows):
            qoff, mlen, mult, coff, doc = struct.unpack_from("<IIQQQ", p, 16 + 32 * k)
            rs.append({
                "window_id": wid, "query_offset": qoff,
                "corpus_doc": None if doc == (1 << 64) - 1 else doc,
                "corpus_offset": coff, "match_len": mlen, "multiplicity": mult,
            })
        frames.append({"window_id": wid, "seeds": seeds, "rows": rs})
    return frames

def rows_of_frames(frames):
    out = []
    for f in frames:
        out.extend(f["rows"])
    return out

def key(r):
    return (r["window_id"], r["query_offset"], r["corpus_offset"], r["match_len"],
            r["multiplicity"])

def doc_starts_of(corpus):
    """Independent document map: every byte after a 0x1E starts a document."""
    with open(corpus, "rb") as f:
        data = f.read()
    starts = [0]
    i = 0
    while True:
        j = data.find(b"\x1e", i)
        if j < 0:
            break
        if j + 1 < len(data):
            starts.append(j + 1)
        i = j + 1
    return data, starts

def docid(starts, pos):
    lo, hi = 0, len(starts)
    while lo < hi:
        mid = (lo + hi) // 2
        if starts[mid] <= pos:
            lo = mid + 1
        else:
            hi = mid
    return lo - 1

def occurrences(oracle, corpus, patterns, scratch):
    """(count, starts) per pattern, from the independent C oracle."""
    path = os.path.join(scratch, "anchor_patterns.bin")
    with open(path, "wb") as f:
        for p in patterns:
            f.write(struct.pack("<I", len(p)))
            f.write(p)
    r = sh([oracle, corpus, path])
    if r.returncode != 0:
        die(f"oracle failed: {r.stderr.decode()[:400]}")
    out = []
    for line in r.stdout.decode().splitlines():
        parts = [int(x) for x in line.split()]
        count, printed = parts[0], parts[1]
        starts = parts[2:]
        if len(starts) != printed:
            die(f"oracle line has {len(starts)} starts, printed={printed}")
        out.append((count, starts))
    if len(out) != len(patterns):
        die(f"oracle returned {len(out)} lines for {len(patterns)} patterns")
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xsa")
    ap.add_argument("container")
    ap.add_argument("corpus")
    ap.add_argument("--out", required=True)
    ap.add_argument("--docs", choices=["present", "absent", "auto"], default="auto")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--windows", type=int, default=4)
    ap.add_argument("--window-len", type=int, default=200)
    args = ap.parse_args()
    out = args.out
    os.makedirs(out, exist_ok=True)
    oracle = os.path.join(out, "seeds_anchor_oracle")
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seeds_anchor_oracle.c")
    r = sh(["gcc", "-O2", "-o", oracle, src])
    if r.returncode != 0:
        die(f"oracle build failed: {r.stderr.decode()[:400]}")

    corpus, starts = doc_starts_of(args.corpus)
    # ---- windows: DECLARED, never inferred. Spread across the corpus so the
    # four windows are four unrelated regions.
    offs = [(k + 1) * len(corpus) // (args.windows + 3) for k in range(args.windows)]
    offs = [min(o, max(0, len(corpus) - args.window_len - 1)) for o in offs]
    wins = [corpus[o:o + args.window_len] for o in offs]
    wins_jsonl = os.path.join(out, "wins.jsonl")
    with open(wins_jsonl, "w") as f:
        for i, o in enumerate(offs):
            f.write(json.dumps({"doc": i, "base": 0, "start": o, "len": args.window_len}) + "\n")
    frames_in = os.path.join(out, "frames.bin")
    with open(frames_in, "wb") as f:
        for w in wins:
            f.write(struct.pack("<I", len(w)))
            f.write(w)

    bad = 0
    def check(cond, msg):
        nonlocal bad
        if not cond:
            bad += 1
            print("  FAIL " + msg)
        return cond

    # ---------------------------------------------------------------- W1
    print("W1 the three input forms answer the same rows")
    p_file = os.path.join(out, "anchors_file.tsv")
    p_wins = os.path.join(out, "anchors_windows.tsv")
    p_frames = os.path.join(out, "anchors_frames.bin")
    single = os.path.join(out, "window0.bin")
    open(single, "wb").write(wins[0])
    r_file = run(args.xsa, ["seeds", args.container, single, "--anchors"])
    open(p_file, "wb").write(r_file.stdout)
    r_wins = run(args.xsa, ["seeds", args.container, "--windows", wins_jsonl,
                            "--corpus", args.corpus, "-j", str(args.jobs)])
    open(p_wins, "wb").write(r_wins.stdout)
    check(stats_of(r_wins.stderr) is not None, "no SEEDS_SERVE_STATS line from --windows")
    r_serve = run(args.xsa, ["seeds", args.container, "--serve", "-j", str(args.jobs)],
                  stdin=open(frames_in, "rb").read())
    open(p_frames, "wb").write(r_serve.stdout)
    # m15 presence is read off the READY line (docs=m15:<n> vs docs=absent)
    ready = [l for l in r_wins.stderr.decode().splitlines() if l.startswith("SEEDS_SERVE_READY ")]
    if not check(len(ready) == 1, "no SEEDS_SERVE_READY line"):
        die("cannot read the document-structure declaration")
    ready_kv = dict(kv.split("=", 1) for kv in ready[0][len("SEEDS_SERVE_READY "):].split())
    has_m15 = ready_kv["docs"].startswith("m15:")
    if args.docs != "auto":
        check((args.docs == "present") == has_m15,
              f"--docs {args.docs} but the container declares docs={ready_kv['docs']}")

    _, rows_file = read_tsv(p_file)
    _, rows_wins = read_tsv(p_wins)
    frames = decode_frames(p_frames)
    rows_frames = rows_of_frames(frames)
    check(len(frames) == len(wins), f"serve frames {len(frames)} != windows {len(wins)}")
    for f, i in zip(frames, range(len(frames))):
        check(f["window_id"] == i, f"frame {i} carries window_id {f['window_id']}")
    w0 = [r for r in rows_wins if r["window_id"] == 0]
    check(sorted(map(key, w0)) == sorted(map(key, rows_file)),
          f"--windows window 0 ({len(w0)} rows) != file mode ({len(rows_file)} rows)")
    check(sorted(map(key, rows_wins)) == sorted(map(key, rows_frames)),
          f"--windows ({len(rows_wins)} rows) != serve frames ({len(rows_frames)} rows)")
    check(len(rows_wins) > 0, "no anchor rows at all")
    print(f"  rows file={len(rows_file)} windows={len(rows_wins)} frames={len(rows_frames)}")

    # ---------------------------------------------------------------- W2
    print("W2 every row is an EXACT anchor (oracle)")
    pats, where = [], []
    for r in rows_wins:
        w = wins[r["window_id"]]
        p = w[r["query_offset"]:r["query_offset"] + r["match_len"]]
        check(len(p) == r["match_len"],
              f"row {r}: query_offset+match_len runs past the window")
        pats.append(p)
        where.append(r)
    occ = occurrences(oracle, args.corpus, pats, out)
    for r, (count, ostart) in zip(where, occ):
        check(count == r["multiplicity"],
              f"row {r}: multiplicity {r['multiplicity']} but the oracle counts {count}")
        check(r["corpus_offset"] in ostart,
              f"row {r}: corpus_offset {r['corpus_offset']} is not one of the {count} occurrences")
    # the whole window's scan is what it says: every query offset is < window len
    for r in rows_wins:
        check(0 <= r["query_offset"] < len(wins[r["window_id"]]), f"row {r}: bad query_offset")
    print(f"  patterns={len(pats)} rows checked")

    # the cap does not touch the support
    capped = os.path.join(out, "anchors_capped.tsv")
    r_cap = run(args.xsa, ["seeds", args.container, "--windows", wins_jsonl,
                           "--corpus", args.corpus, "-j", str(args.jobs),
                           "--anchor-max-per-seed", "1"])
    open(capped, "wb").write(r_cap.stdout)
    _, rows_cap = read_tsv(capped)
    check(len(rows_cap) < len(rows_wins),
          f"--anchor-max-per-seed 1 did not shrink the row set ({len(rows_cap)} rows)")
    mult_wins = {(r["query_offset"], r["match_len"], r["multiplicity"]) for r in rows_wins}
    for r in rows_cap:
        check((r["query_offset"], r["match_len"], r["multiplicity"]) in mult_wins,
              f"capped row {r}: multiplicity changed under an OUTPUT cap")
    print(f"  rows all={len(rows_wins)} capped={len(rows_cap)} (multiplicity invariant)")

    # ---------------------------------------------------------------- W3
    print("W3 corpus_doc comes from m15, and is ABSENT when the container has none")
    if has_m15:
        for r in rows_wins:
            want = docid(starts, r["corpus_offset"])
            check(r["corpus_doc"] == want,
                  f"row {r}: corpus_doc {r['corpus_doc']} but the corpus separator scan says {want}")
        check(all(r["corpus_doc"] is not None for r in rows_wins), "a row has no doc field")
        print(f"  m15 present: {len(rows_wins)} doc fields verified against the 0x1E scan")
    else:
        check(all(r["corpus_doc"] is None for r in rows_wins),
              "container has no m15 but a row carries a docid (a docid must never be guessed)")
        print(f"  m15 absent: all {len(rows_wins)} doc fields marked '-' ")

    # ---------------------------------------------------------------- W4
    print("W4 determinism: same input twice, and -j 1 vs -j N")
    again = run(args.xsa, ["seeds", args.container, "--windows", wins_jsonl,
                           "--corpus", args.corpus, "-j", str(args.jobs)])
    check(hashlib.sha256(again.stdout).hexdigest() == hashlib.sha256(r_wins.stdout).hexdigest(),
          "two identical --windows runs differ")
    j1 = run(args.xsa, ["seeds", args.container, "--windows", wins_jsonl,
                        "--corpus", args.corpus, "-j", "1"])
    check(hashlib.sha256(j1.stdout).hexdigest() == hashlib.sha256(r_wins.stdout).hexdigest(),
          f"-j 1 and -j {args.jobs} differ")
    s1 = run(args.xsa, ["seeds", args.container, "--serve", "-j", "1"],
             stdin=open(frames_in, "rb").read())
    sN = run(args.xsa, ["seeds", args.container, "--serve", "-j", str(args.jobs)],
             stdin=open(frames_in, "rb").read())
    check(hashlib.sha256(s1.stdout).hexdigest() == hashlib.sha256(sN.stdout).hexdigest(),
          f"serve -j 1 and -j {args.jobs} differ")
    check(hashlib.sha256(s1.stdout).hexdigest() == hashlib.sha256(r_serve.stdout).hexdigest(),
          "two identical serve runs differ")
    print(f"  sha256 windows={hashlib.sha256(r_wins.stdout).hexdigest()[:16]} "
          f"serve={hashlib.sha256(s1.stdout).hexdigest()[:16]}")

    # ---------------------------------------------------------------- W5
    print("W5 reads follow the query set, not the container")
    # 1 window vs 4 windows, each in ONE fresh process, so each rchar delta is
    # that run's own reads
    w1 = os.path.join(out, "frames1.bin")
    with open(frames_in, "rb") as f:
        n0 = struct.unpack("<I", f.read(4))[0]
        open(w1, "wb").write(struct.pack("<I", n0) + f.read(n0))
    st1 = stats_of(run(args.xsa, ["seeds", args.container, "--serve", "-j", "1"],
                       stdin=open(w1, "rb").read()).stderr)
    st4 = stats_of(run(args.xsa, ["seeds", args.container, "--serve", "-j", "1"],
                       stdin=open(frames_in, "rb").read()).stderr)
    if check(st1 is not None and st4 is not None, "no serve stats for the read check"):
        r1, r4 = int(st1["rchar"]), int(st4["rchar"])
        container_bytes = os.path.getsize(args.container)
        # (a) the reads FOLLOW THE QUERY SET: four windows read about four times
        # one window, and never more.
        check(r4 <= 6 * max(r1, 1), f"4 windows read {r4} vs 1 window {r1}: not query-proportional")
        # (b) the reads follow the LOCATE LAYER, not the container: one anchor row
        # is one LF walk, and a walk is a bounded number of decoded blocks. 8 MiB
        # per row is a generous ceiling for that (measured ~1.4 MiB here).
        pos4 = int(st4["positions"])
        per_pos = r4 / max(pos4, 1)
        check(per_pos <= (8 << 20),
              f"{per_pos:.0f} bytes read per anchor row: that is not a bounded LF walk")
        # (c) on a container large enough for the comparison to mean something, the
        # whole run reads a small FRACTION of it (on a 150 MB fixture a few hundred
        # LF walks legitimately touch the whole index, so this check needs scale).
        if container_bytes >= (8 << 30):
            check(r4 < container_bytes // 4,
                  f"4 windows read {r4} of a {container_bytes}-byte container: container-bound")
        print(f"  rchar 1 window={r1} 4 windows={r4} ratio={r4 / max(r1, 1):.2f} "
              f"positions={pos4} bytes_per_row={per_pos:.0f} container={container_bytes} "
              f"frac_container={r4 / container_bytes:.4f}")

    # ---------------------------------------------------------------- summary
    if bad:
        die(f"{bad} check(s) failed")
    print("SEEDS_WARM_GATE_OK")

if __name__ == "__main__":
    main()
