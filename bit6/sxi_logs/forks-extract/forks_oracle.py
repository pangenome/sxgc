#!/usr/bin/env python3
"""Independent structural oracle for `xsa forks` on a small FASTA corpus.

Takes the ARTIFACT's chi set (exported via `xsa sxi-info --chi-out`) as the
witness ground truth (it was produced by the production cyclic sweep and
gated at build time; the extractor must consume exactly that set), then
recomputes, from the corpus bytes alone:
  - the cyclic reversed-stream SA, BWT, runs;
  - a check that the artifact's own head/tail SA samples agree with the
    corpus-derived SA (validates the ssa/ssa_t conventions);
  - per witness: the witnessing junction runs, classes (distinct BWT chars),
    and strings (records containing each occurrence's branch-char position).
Diffs the result against the emitted JSONL record for record.
"""
import json
import struct
import sys
from collections import defaultdict

SEP = 0x1E


def read_fasta(path):
    recs, name, seq = [], None, []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith(">"):
                if name is not None:
                    recs.append((name, "".join(seq)))
                name, seq = line[1:], []
            elif line:
                seq.append(line)
    if name is not None:
        recs.append((name, "".join(seq)))
    return recs


def main(fasta, chi_path, sxi_path, jsonl):
    recs = read_fasta(fasta)
    S = bytearray()
    bounds = []
    for _, r in recs:
        bounds.append((len(S), len(r)))
        S.extend(r.encode())
        S.append(SEP)
    S = bytes(S)
    n = len(S)
    # The artifact indexes the FORWARD cyclic stream (validated: its run
    # table equals the forward-BWT runs; the forward-cyclic FM scan
    # reproduces its chi exactly).
    k = 1
    rank = [S[i] for i in range(n)]
    sa = list(range(n))
    tmp = [0] * n
    while True:
        key = lambda i: (rank[i], rank[(i + k) % n])
        sa.sort(key=key)
        tmp[sa[0]] = 0
        for j in range(1, n):
            tmp[sa[j]] = tmp[sa[j - 1]] + (key(sa[j - 1]) < key(sa[j]))
        rank = tmp[:]
        if rank[sa[-1]] == n - 1:
            break
        k *= 2
    bwt = [S[(i - 1) % n] for i in sa]
    runs = []
    s = 0
    while s < n:
        e = s
        while e + 1 < n and bwt[e + 1] == bwt[s]:
            e += 1
        runs.append((bwt[s], s, e - s + 1))
        s = e + 1
    nrun = len(runs)
    runid = [0] * n
    for r, (ch, st, ln) in enumerate(runs):
        for j in range(st, st + ln):
            runid[j] = r

    # artifact chi
    chi = []
    with open(chi_path, "rb") as f:
        b = f.read()
    for i in range(0, len(b), 8):
        chi.append(struct.unpack_from("<Q", b, i)[0])
    assert all(a < b_ for a, b_ in zip(chi, chi[1:]))

    # artifact samples (SXI1: members 3 raw heads; 2 packed mirrored tails)
    with open(sxi_path, "rb") as f:
        h = f.read(64)
        cnt = struct.unpack_from("<I", h, 32)[0]
        d = f.read(40 * cnt)
        members = {}
        for i in range(cnt):
            mid, codec, off, nbytes, mcount = struct.unpack_from("<IIQQQ", d, 40 * i)
            members[mid] = (off, nbytes, mcount)
        off3, _, r3 = members[3]
        f.seek(off3)
        heads = struct.unpack_from(f"<{r3}Q", f.read(8 * r3))
        off2, _, _ = members[2]
        f.seek(off2)
        hb = f.read(9)
        bits = struct.unpack_from("<Q", hb, 0)[0]
        w = hb[8]
        words = (bits + 63) // 64
        packed = f.read(words * 8)
        tails = []
        res, have, wi = 0, 0, 0
        for _ in range(r3):
            while have < w:
                res |= struct.unpack_from("<Q", packed, wi)[0] << have
                wi += 8
                have += 64
            v = res & ((1 << w) - 1)
            tails.append(n - 1 - v)
            res >>= w
            have -= w

    # Check the artifact's samples against the corpus SA.
    bad = 0
    for ridx, (ch, st, ln) in enumerate(runs):
        if heads[ridx] != sa[st] or tails[ridx] != sa[st + ln - 1]:
            if bad < 5:
                print(f"sample mismatch run {ridx}: artifact head {heads[ridx]} tail {tails[ridx]}"
                      f" vs corpus sa {sa[st]} / {sa[st+ln-1]}")
            bad += 1
    print(f"artifact ssa/ssa_t samples checked against corpus SA: "
          f"{'OK' if bad == 0 else f'{bad} MISMATCHES'} over {nrun} runs")

    def record_of(pos):
        p = pos % n
        for i, (st, ln) in enumerate(bounds):
            if st <= p <= st + ln:
                return i
        raise AssertionError(p)

    def strings_of_run(run):
        st, ln = runs[run][1], runs[run][2]
        out = set()
        for j in range(st, st + ln):
            out.add(record_of((sa[j] - 1) % n))  # BWT char position: S[sa-1]
        return sorted(out)

    x_to_row = {}
    for i in range(n):
        x_to_row[(n - sa[i]) % n] = i

    expected = {}
    for x in chi:
        row = x_to_row[x]
        r = runid[row]
        is_head = runs[r][1] == row
        is_tail = runs[r][1] + runs[r][2] - 1 == row
        if not (is_head or is_tail):
            print(f"FAIL witness {x}: row {row} is not a run-edge row (run {r} spans "
                  f"{runs[r][1]}..{runs[r][1]+runs[r][2]-1})")
            return
        if is_head and is_tail:
            adj = [(r - 1) % nrun, r, (r + 1) % nrun]
        elif is_head:
            adj = [(r - 1) % nrun, r]
        else:
            adj = [r, (r + 1) % nrun]
        classes = []
        for run in adj:
            ch = runs[run][0]
            hit = [c for c, _ in classes if c == ch]
            if hit:
                classes[[i for i, (c, _) in enumerate(classes) if c == ch][0]][1].append(run)
            else:
                classes.append((ch, [run]))
        recs_out = []
        for cid, (ch, rlist) in enumerate(classes):
            strings = sorted({s for run in rlist for s in strings_of_run(run)})
            recs_out.append((cid, ch, strings))
        expected[x] = recs_out

    got = defaultdict(list)
    with open(jsonl) as f:
        for line in f:
            o = json.loads(line)
            got[o["witness"]].append((o["class_id"], o["char"], o["strings"]))
    ok = True
    if set(got) != set(expected):
        print(f"FAIL witness sets differ: extractor {len(got)} vs oracle {len(expected)}")
        ok = False
    nbad = 0
    for x in sorted(set(got) & set(expected)):
        if got[x] != expected[x]:
            nbad += 1
            if nbad <= 3:
                print(f"FAIL witness {x}:")
                print(f"   got      {got[x]}")
                print(f"   expected {expected[x]}")
                row = x_to_row[x]
                r = runid[row]
                print(f"   witness row {row}, run {r} {runs[r]}, adj per oracle")
    if nbad:
        ok = False
        print(f"{nbad} mismatching witnesses")
    multi = sum(1 for x in expected for (_, _, ss) in expected[x] if len(ss) > 1)
    overlap = sum(
        1 for x in expected for i in range(len(expected[x]))
        for j in range(len(expected[x]))
        if i != j and set(expected[x][i][2]) & set(expected[x][j][2]))
    print(f"oracle: {len(expected)} witnesses; multi-string classes {multi}; "
          f"cross-class string overlaps {overlap}")
    print("VERDICT:", "MATCH" if ok else "MISMATCH")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4])
