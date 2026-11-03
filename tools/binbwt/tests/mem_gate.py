#!/usr/bin/env python3
"""X1/X2/X3 gates for `xsa mem` (maximal exact matches).

X1  brute force: every query's MEM set is computed INDEPENDENTLY from corpus
    bytes by tools/binbwt/tests/mem_oracle.c (one mmap pass, no index code, no
    shared implementation) and must agree exactly with `xsa mem --all-mems` on
    (query_start, query_len, occurrences) - and with the default single-longest
    answer on (query_start, query_len, occurrences).
X2  edge cases: empty query refused, absent query (best_len 0), query present in
    full, --min-len above every match, and a 4 KB query.
X3  positions: --docs --limit 8 resolves doc_id/offset/title, spot-checked
    against the corpus bytes.

Usage:
  mem_gate.py XSA CONTAINER CORPUS --out DIR [--scale 150m|100g] [--min-len 8]
              [--docs DIR] [--serve-jobs N] [--queries N] [--skip-edge]
"""
import argparse
import json
import os
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def die(msg):
    sys.stderr.write("mem_gate: FAIL: %s\n" % msg)
    sys.exit(1)


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def oracle_binary(workdir):
    src = os.path.join(HERE, "mem_oracle.c")
    out = os.path.join(workdir, "mem_oracle")
    r = run(["gcc", "-O2", "-march=native", "-o", out, src])
    if r.returncode != 0:
        die("oracle build failed: %s" % r.stderr.decode()[:2000])
    return out


def write_queries(path, queries):
    with open(path, "wb") as f:
        for q in queries:
            f.write(struct.pack("<I", len(q)))
            f.write(q)


def oracle_mems(binary, corpus, queries, min_len, workdir):
    """True MEM set per query: {(start, len, occ)}, plus the longest MEM."""
    qpath = os.path.join(workdir, "queries.bin")
    write_queries(qpath, queries)
    r = run([binary, corpus, qpath, str(min_len)])
    if r.returncode != 0:
        die("oracle failed: %s" % r.stderr.decode()[:2000])
    sys.stderr.write(r.stderr.decode())
    lcp = {}          # qid -> {a: (lcp, occ)}
    qlen = {}
    for line in r.stdout.decode().splitlines():
        f = line.split()
        if f[0] == "Q":
            qlen[int(f[1])] = int(f[2])
        elif f[0] == "A":
            lcp.setdefault(int(f[1]), {})[int(f[2])] = (int(f[3]), int(f[4]))
        else:
            die("oracle output: %r" % line)
    out = []
    for i, q in enumerate(queries):
        assert qlen[i] == len(q)
        hits = lcp.get(i, {})
        mems = set()
        for a in sorted(hits):
            L, occ = hits[a]
            if L < min_len:
                continue
            # left-maximal: query[a-1 .. a+L-1] must not occur
            prev = hits.get(a - 1)
            if a > 0 and prev is not None and prev[0] > L:
                continue
            mems.add((a, L, occ))
        best = None
        for a, L, occ in mems:
            if best is None or (L, -a) > (best[1], -best[0]):
                best = (a, L, occ)
        out.append({"mems": mems, "best": best})
    return out


def index_all_mems(xsa, container, query, min_len, docs=None):
    # BYTES args: str would be fsencoded as UTF-8 and every byte >= 0x80 would
    # become two bytes, so the index would search a different query than the
    # oracle. The CLI takes the query verbatim.
    cmd = [xsa, "mem", container, query, b"--all-mems",
           b"--min-len", str(min_len).encode(), b"--limit", b"0", b"--jsonl"]
    if docs:
        cmd += ["--docs", docs]
    r = run(cmd)
    if r.returncode != 0:
        die("xsa mem failed for %r: %s" % (query[:40], r.stderr.decode()[-2000:]))
    mems = set()
    for line in r.stdout.decode().splitlines():
        o = json.loads(line)
        mems.add((o["query_start"], o["query_len"], o["occurrences"]))
    return mems


def index_best(xsa, container, query, min_len, docs=None):
    cmd = [xsa, "mem", container, query, b"--min-len",
           str(min_len).encode(), b"--limit", b"0", b"--jsonl"]
    if docs:
        cmd += ["--docs", docs]
    r = run(cmd)
    if r.returncode != 0:
        die("xsa mem failed for %r: %s" % (query[:40], r.stderr.decode()[-2000:]))
    lines = [json.loads(l) for l in r.stdout.decode().splitlines()]
    if not lines:
        return None
    o = lines[0]
    return (o["query_start"], o["query_len"], o["occurrences"])


def serve_best(xsa, container, queries, min_len, jobs, docs=None):
    """One process, framed mode: [(query_start, best_len, occurrences)]."""
    cmd = [xsa, "mem", container, "--serve", "-j", str(jobs), "--min-len", str(min_len)]
    if docs:
        cmd += ["--docs", docs]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE)
    payload = b"".join(struct.pack("<I", len(q)) + q for q in queries)
    out, err = p.communicate(payload)
    if p.returncode != 0:
        die("mem --serve failed: %s" % err.decode()[-2000:])
    sys.stderr.write(err.decode())
    res, off = [], 0
    while off + 4 <= len(out):
        n = struct.unpack("<I", out[off:off + 4])[0]
        off += 4
        body = out[off:off + n]
        off += n
        if n != 16:
            die("frame payload %d != 16" % n)
        best_len, qs, occ = struct.unpack("<IIQ", body)
        res.append((qs, best_len, occ))
    if off != len(out) or len(res) != len(queries):
        die("frame count %d != %d" % (len(res), len(queries)))
    return res


def make_queries(corpus_path, count, seed, long_bytes=32):
    """A mix: sampled substrings that exist, brand-new strings, a repeated base,
    one full-corpus-absent string."""
    rng = random.Random(seed)
    f = open(corpus_path, "rb")
    n = os.path.getsize(corpus_path)
    mm = __import__("mmap").mmap(f.fileno(), 0, access=__import__("mmap").ACCESS_READ)
    qs = []
    labels = []
    # order: sampled, brand-new, repeated base, absent; round-robin so a short
    # run still covers every class.
    kinds = ["sampled", "mutated", "split2", "brandnew", "repeat", "absent"]
    safe = [b for b in range(1, 256)]  # argv cannot carry NUL

    def window(pos, ln):
        return bytes(mm[pos:pos + ln])

    for i in range(count):
        kind = kinds[i % len(kinds)] if i < len(kinds) else rng.choice(kinds)
        if kind in ("sampled", "mutated", "split2"):
            for _ in range(10000):
                pos = rng.randrange(0, n - long_bytes - 1)
                q = window(pos, long_bytes)
                if b"\x00" not in q:
                    break
            else:
                die("could not sample a NUL-free window")
            if kind == "mutated":
                # one byte in the middle changed: two maximal matches, one on
                # each side, both bounded by a mismatch -> exercises both
                # maximality tests against a non-trivial answer.
                mid = long_bytes // 2
                q = q[:mid] + bytes([q[mid] ^ 0xFF]) + q[mid + 1:]
                if b"\x00" in q:
                    q = q[:mid] + b"~" + q[mid + 1:]
                labels.append("mutated@%d" % pos)
            elif kind == "split2":
                for _ in range(10000):
                    pos2 = rng.randrange(0, n - long_bytes - 1)
                    tail = window(pos2, long_bytes // 2)
                    if b"\x00" not in tail:
                        break
                q = q[:long_bytes // 2] + tail
                labels.append("split2@%d+%d" % (pos, pos2))
            else:
                labels.append("sampled@%d" % pos)
            qs.append(q)
        elif kind == "brandnew":
            alpha = b"abcdefghijklmnopqrstuvwxyz "
            qs.append(bytes(rng.choice(alpha) for _ in range(long_bytes)))
            labels.append("brandnew")
        elif kind == "repeat":
            base = bytes(rng.choice(b"acgtxy") for _ in range(rng.randrange(2, 5)))
            qs.append((base * (long_bytes // len(base) + 1))[:long_bytes])
            labels.append("repeat:%r" % base)
        else:
            # a full-corpus-absent string: 30 pseudo-random bytes, no NUL
            qs.append(bytes(rng.choice(safe) for _ in range(long_bytes)))
            labels.append("absent-binary")
    assert all(b"\x00" not in q for q in qs), "query carries NUL"
    mm.close()
    f.close()
    return qs, labels


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xsa")
    ap.add_argument("container")
    ap.add_argument("corpus")
    ap.add_argument("--out", required=True)
    ap.add_argument("--scale", default="150m")
    ap.add_argument("--min-len", type=int, default=8)
    ap.add_argument("--docs")
    ap.add_argument("--serve-jobs", type=int, default=16)
    ap.add_argument("--queries", type=int, default=20)
    ap.add_argument("--seed", type=int, default=20261104)
    ap.add_argument("--skip-edge", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="memgate-")
    t0 = time.time()
    report = {"scale": a.scale, "min_len": a.min_len, "container": a.container,
              "corpus": a.corpus}

    # ---------------- X1 ----------------
    queries, labels = make_queries(a.corpus, a.queries, a.seed)
    binary = oracle_binary(work)
    print("[X1] oracle pass over %s" % a.corpus, flush=True)
    want = oracle_mems(binary, a.corpus, queries, a.min_len, work)
    agree_all = agree_best = 0
    rows = []
    if a.scale == "150m":
        for i, q in enumerate(queries):
            got_all = index_all_mems(a.xsa, a.container, q, a.min_len)
            exp_all = want[i]["mems"]
            ok_all = got_all == exp_all
            got_best = index_best(a.xsa, a.container, q, a.min_len)
            exp_best = want[i]["best"]
            ok_best = got_best == exp_best
            agree_all += ok_all
            agree_best += ok_best
            rows.append({"i": i, "label": labels[i], "query_hex": q.hex(),
                         "expected_all": sorted(exp_all), "got_all": sorted(got_all),
                         "expected_best": exp_best, "got_best": got_best,
                         "all_ok": ok_all, "best_ok": ok_best})
            print("[X1] q%-2d %-14s all=%s(%d) best=%s" %
                  (i, labels[i], "OK" if ok_all else "MISMATCH", len(exp_all),
                   "OK" if ok_best else "MISMATCH"), flush=True)
            if not ok_all:
                print("      expected %s\n      got      %s" % (sorted(exp_all), sorted(got_all)))
    else:
        # One open for all queries: framed mode is the only path that reports the
        # longest match per request, and it is what X4 measures.
        got = serve_best(a.xsa, a.container, queries, a.min_len, a.serve_jobs)
        for i, q in enumerate(queries):
            exp = want[i]["best"]
            exp3 = (exp[0], exp[1], exp[2]) if exp else (0, 0, 0)
            got3 = (got[i][0], got[i][1], got[i][2])
            ok = exp3 == got3
            agree_best += ok
            agree_all += ok
            rows.append({"i": i, "label": labels[i], "query_hex": q.hex(),
                         "expected_best": exp3, "got_best": got3, "best_ok": ok})
            print("[X1] q%-2d %-14s best=%s expected=%s got=%s" %
                  (i, labels[i], "OK" if ok else "MISMATCH", exp3, got3), flush=True)
    report["X1"] = {"queries": len(queries), "agree_all_mems": agree_all,
                    "agree_best": agree_best, "rows": rows}

    # ---------------- X2 ----------------
    if not a.skip_edge:
        edges = {}
        r = run([a.xsa, "mem", a.container, b""])
        edges["empty_query_refused"] = (r.returncode != 0 and
                                        b"empty query" in r.stderr and
                                        b"at least one byte" in r.stderr)
        absent = bytes(range(240, 250)) * 3  # 30 bytes, no NUL
        r = run([a.xsa, "mem", a.container, absent,
                 b"--min-len", b"8", b"--limit", b"0", b"--jsonl"])
        edges["absent_best_len_0"] = (r.returncode == 0 and r.stdout.strip() == b"")
        # a query present in full: sample from the middle of the corpus
        n = os.path.getsize(a.corpus)
        f = open(a.corpus, "rb")
        mm = __import__("mmap").mmap(f.fileno(), 0, access=__import__("mmap").ACCESS_READ)
        for _ in range(100000):
            pos = n // 3
            q = bytes(mm[pos:pos + 40])
            if b"\x00" not in q:
                break
        mm.close()
        f.close()
        got = index_best(a.xsa, a.container, q, 8)
        edges["present_in_full"] = got is not None and got[1] == 40
        r = run([a.xsa, "mem", a.container, b"the", b"--min-len", b"4096",
                 b"--limit", b"0", b"--jsonl"])
        edges["min_len_above_all"] = (r.returncode == 0 and r.stdout.strip() == b"")
        # 4 KB query: a sampled window, so the match is the whole query
        w = 4096
        f = open(a.corpus, "rb")
        mm = __import__("mmap").mmap(f.fileno(), 0, access=__import__("mmap").ACCESS_READ)
        for _ in range(100000):
            pos = n // 2
            q4 = bytes(mm[pos:pos + w])
            if b"\x00" not in q4:
                break
        mm.close()
        f.close()
        t = time.time()
        got4 = index_best(a.xsa, a.container, q4, 8)
        edges["query_4kb_full_match"] = got4 is not None and got4[1] == w
        edges["query_4kb_seconds"] = round(time.time() - t, 3)
        report["X2"] = edges
        print("[X2] %s" % json.dumps(edges), flush=True)
        if not all(v for k, v in edges.items() if isinstance(v, bool)):
            die("X2 edge cases failed: %s" % json.dumps(edges))

    # ---------------- X3 ----------------
    if a.docs:
        cq = b"cat sat on the mat"
        n = os.path.getsize(a.corpus)
        out = run([a.xsa, "mem", a.container, cq, b"--docs", a.docs,
                   b"--limit", b"8", b"--jsonl", b"--min-len", b"8"])
        if out.returncode != 0:
            die("X3 mem failed: %s" % out.stderr.decode()[-1500:])
        rows3 = [json.loads(l) for l in out.stdout.decode().splitlines()]
        if not rows3:
            die("X3: no MEM for %r" % cq)
        top = rows3[0]
        docs = top.get("docs") or []
        if not docs:
            die("X3: sidecars did not resolve (no docs array)")
        f = open(a.corpus, "rb")
        mm = __import__("mmap").mmap(f.fileno(), 0, access=__import__("mmap").ACCESS_READ)
        checks = []
        for d in docs[:3]:
            sub = cq[top["query_start"]:top["query_start"] + top["query_len"]]
            at = mm[d["pos"]:d["pos"] + top["query_len"]]
            checks.append({"doc_id": d["doc_id"], "offset": d["offset"], "title": d["title"],
                           "pos": d["pos"], "bytes_match": at == sub,
                           "title_prefix_in_doc": mm[d["pos"] - d["offset"]:
                                                      d["pos"] - d["offset"] + min(40, len(d["title"]))] ==
                                                  d["title"].encode()[:min(40, len(d["title"]))]})
        mm.close()
        f.close()
        report["X3"] = {"mem": top, "spot_checks": checks}
        print("[X3] %s" % json.dumps(checks), flush=True)
        if not all(c["bytes_match"] and c["title_prefix_in_doc"] for c in checks):
            die("X3 position spot-checks failed: %s" % json.dumps(checks))

    report["seconds"] = round(time.time() - t0, 1)
    path = os.path.join(a.out, "mem_gate_%s.json" % a.scale)
    with open(path, "w") as f:
        json.dump(report, f, indent=1)
    print("[gate] wrote %s" % path)
    if a.scale == "150m":
        print("[X1] VERDICT all-mems %d/%d, longest %d/%d" %
              (agree_all, len(queries), agree_best, len(queries)))
        if agree_all != len(queries) or agree_best != len(queries):
            die("X1 disagreement at 150 MB")
    else:
        print("[X1] VERDICT longest %d/%d at %s" % (agree_best, len(queries), a.scale))
        if agree_best != len(queries):
            die("X1 disagreement at %s" % a.scale)
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
