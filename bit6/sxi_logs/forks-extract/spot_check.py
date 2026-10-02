#!/usr/bin/env python3
"""Gate (a) spot-check: verify sampled witnesses of the full yeast fork
matrix by DIRECT TEXT CHECK against the retained corpus.

Independence from the extractor:
  - the φ domain table is decoded here from member 8 with numpy (a different
    implementation from both the extractor's chain and xsa's Phi::successor);
  - run characters are READ FROM THE CORPUS at (head SA - 1) mod n, never
    from the artifact's run table;
  - run row extents are recovered by walking φ from the ssa head sample to
    the ssa_t tail sample (no run lengths are trusted);
  - every branch-character position of every walked row is read from the
    corpus bytes and checked (char == class char; containing record ==
    claimed string id);
  - the names sidecar is byte-compared against artifact member 6.
The emitted string sets must match exactly.
"""
import json
import mmap
import struct
import sys
import numpy as np

SEP = 0x1E


def bits_lsb(data):
    return np.unpackbits(np.frombuffer(data, dtype=np.uint8), bitorder="little")


def packed_values(bitarray, count, width):
    if width == 0:
        return np.zeros(count, dtype=np.uint64)
    v = bitarray[: count * width].reshape(count, width).astype(np.uint64)
    w = (1 << np.arange(width, dtype=np.uint64)).astype(np.uint64)
    return v @ w


def main(sxi_path, names_path, corpus_path, chi_path, jsonl_path, ncheck):
    ncheck = int(ncheck)
    ncheck = int(ncheck)
    with open(sxi_path, "rb") as f:
        h = f.read(64)
        assert h[:4] == b"SXI2"
        n, k, r = struct.unpack_from("<QQQ", h, 8)
        cnt = struct.unpack_from("<I", h, 32)[0]
        d = f.read(40 * cnt)
        members = {}
        for i in range(cnt):
            mid, codec, off, nbytes, mcount = struct.unpack_from("<IIQQQ", d, 40 * i)
            members[mid] = (off, nbytes, mcount)
        m6 = members[6]
        f.seek(m6[0])
        artifact_names = f.read(m6[1])
    print(f"artifact: n={n} k={k} r={r} chi={members[5][2]}")

    side_names = open(names_path, "rb").read()
    assert side_names == artifact_names, "names.tsv differs from artifact member 6"
    recs = []
    for line in side_names.decode().splitlines():
        name, fstart, length = line.split("\t")
        recs.append((n - 1 - int(fstart) - int(length), int(length)))
    recs.sort()
    starts = np.array([s for s, _ in recs], dtype=np.uint64)
    assert len(recs) == k
    nxt = np.append(starts[1:], n)
    print(f"names: {k} records partition the stream; sidecar == member 6")

    def record_at(p):
        return int(np.searchsorted(starts, p, side="right") - 1)

    # ---- phi domain table (member 8, codec 118), decoded independently
    off, nbytes, mcount = members[8]
    with open(sxi_path, "rb") as f:
        f.seek(off)
        vw, rw, ul, lb, hb = struct.unpack_from("<QQQQQ", f.read(40))
        low_bytes, high_bytes = (lb + 7) // 8, (hb + 7) // 8
        run_bytes, v_bytes = (r * rw + 7) // 8, (r * vw + 7) // 8
        assert nbytes == 40 + low_bytes + high_bytes + run_bytes + v_bytes
        f.seek(off + 40)
        low = bits_lsb(f.read(low_bytes))
        high = bits_lsb(f.read(high_bytes))
        vbits = bits_lsb(f.read(v_bytes))
        runbits = bits_lsb(f.read(run_bytes))
    ones = np.flatnonzero(high[:hb].astype(bool))
    assert len(ones) == mcount == r
    idx = np.arange(r, dtype=np.uint64)
    u = ((ones.astype(np.uint64) - idx) << ul) | packed_values(low, r, ul)
    run_ids = packed_values(runbits, r, rw)
    v = packed_values(vbits, r, vw)
    tail_sa = np.zeros(r, dtype=np.uint64)
    head_sa = np.zeros(r, dtype=np.uint64)
    tail_sa[run_ids] = u
    head_sa[(run_ids + 1) % r] = v
    order = np.argsort(u, kind="stable")
    assert (np.diff(np.sort(u)) > 0).all(), "member 8 u not strictly increasing"
    phi_u = np.sort(u)
    phi_v = v[order]
    print(f"member 8 decoded: {r} phi edges (independent decoder)")

    head_sorted = np.sort(head_sa)
    head_run = np.argsort(head_sa, kind="stable")
    tail_sorted = np.sort(tail_sa)
    tail_run = np.argsort(tail_sa, kind="stable")

    def phi(p):
        i = np.searchsorted(phi_u, p, side="right") - 1
        if i < 0:
            i = r - 1
        return (int(phi_v[i]) + (p + n - int(phi_u[i])) % n) % n

    corpus_file = open(corpus_path, "rb")
    corpus = mmap.mmap(corpus_file.fileno(), 0, access=mmap.ACCESS_READ)
    assert len(corpus) == n, "corpus size != n"

    def S(p):
        return corpus[p % n]

    # run char read from the corpus at the head's branch position
    run_char = {}
    def char_of(run):
        if run not in run_char:
            run_char[run] = S((int(head_sa[run]) + n - 1) % n)
        return run_char[run]

    # ---- sampled witnesses from the exported sorted chi
    chi = np.fromfile(chi_path, dtype="<u8")
    assert len(chi) == members[5][2]
    step = max(1, len(chi) // ncheck)
    sample = sorted(set(int(chi[i]) for i in range(0, len(chi), step)))
    while len(sample) > ncheck:
        sample.pop()
    print(f"sampling {len(sample)} witnesses spread over {len(chi)}")

    got = {}
    with open(jsonl_path) as f:
        for line in f:
            o = json.loads(line)
            x = o["witness"]
            if x in sample:
                got.setdefault(x, []).append((o["class_id"], o["char"], o["strings"]))
            if len(got) == len(sample) and x > sample[-1]:
                break
    missing = [x for x in sample if x not in got]
    assert not missing, f"witnesses missing from output: {missing[:5]}"

    nbad = 0
    total_rows = 0
    for x in sample:
        sa = (n - x) % n
        hi = np.searchsorted(head_sorted, sa)
        ti = np.searchsorted(tail_sorted, sa)
        is_head = hi < r and head_sorted[hi] == sa
        is_tail = ti < r and tail_sorted[ti] == sa
        if not (is_head or is_tail):
            print(f"FAIL witness {x}: no run edge at SA {sa}")
            nbad += 1
            continue
        run = int(head_run[hi]) if is_head else int(tail_run[ti])
        adj = (
            [(run - 1) % r, run, (run + 1) % r]
            if is_head and is_tail
            else [(run - 1) % r, run] if is_head else [run, (run + 1) % r]
        )
        classes = []
        for run_i in adj:
            c = char_of(run_i)
            if any(c == cc for cc, _ in classes):
                classes[[i for i, (cc, _) in enumerate(classes) if cc == c][0]][1].append(run_i)
            else:
                classes.append((c, [run_i]))
        expected = []
        for cid, (c, rlist) in enumerate(classes):
            strings = set()
            ok = True
            for run_i in rlist:
                p = int(head_sa[run_i])
                tail = int(tail_sa[run_i])
                for _ in range(n + 1):
                    q = (p + n - 1) % n
                    if S(q) != c:
                        print(f"FAIL witness {x}: corpus byte at branch position {q} "
                              f"is {S(q)} but class char is {c}")
                        ok = False
                        nbad += 1
                        break
                    strings.add(record_at(q))
                    total_rows += 1
                    if p == tail:
                        break
                    p = phi(p)
                else:
                    print(f"FAIL witness {x}: φ walk never reached the tail sample of run {run_i}")
                    ok = False
                    nbad += 1
                if not ok:
                    break
            expected.append((cid, c, sorted(strings)))
        own = S((n + n - x - 1) % n)
        if own != char_of(run):
            print(f"FAIL witness {x}: own event char {own} != witnessing run char {char_of(run)}")
            nbad += 1
        if got[x] != expected:
            print(f"FAIL witness {x}:\n  got      {got[x]}\n  expected {expected}")
            nbad += 1
    print(f"checked {len(sample)} witnesses, {total_rows} occurrence rows read from the corpus: "
          f"{'ALL PASS' if nbad == 0 else f'{nbad} FAILURES'}")
    print("VERDICT:", "PASS" if nbad == 0 else "FAIL")


if __name__ == "__main__":
    main(*sys.argv[1:])
