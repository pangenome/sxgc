#!/usr/bin/env python3
"""M2 gate-side checker: reconstruct the dense ssa/ssa_t columns from the v2
band pieces and byte-compare against the banked reference; assert exact
run-coverage [0,R). Usage:
  check_pieces.py <prefix> <bank_prefix>   # prefix = merged/frag
"""
import os, struct, sys

prefix, bank = sys.argv[1], sys.argv[2]
with open(prefix + ".runcols.idx", "rb") as f:
    hdr = f.read(32)
    magic, S, R, n, w, B = struct.unpack("<IIQQII", hdr)
    assert magic == 0x31494352, "bad RCI1 magic"
    bases = []
    for s in range(S):
        base, cnt = struct.unpack("<QQ", f.read(16))
        bases.append((base, cnt))
total = sum(c for _, c in bases)
assert total == R, f"idx counts {total} != R {R}"
covered = 0
for base, cnt in bases:
    assert base == covered, f"shard base {base} != running {covered}"
    covered += cnt

pieces_dir = prefix + ".pieces"
# band -> [(shard, firstRun, runCount, chars, lens, heads, tails)]
bands = {}
nfiles = 0
for name in os.listdir(pieces_dir):
    assert name.startswith("p-"), f"stray file {name}"
    band, shard = (int(x) for x in name[2:].split("-"))
    with open(os.path.join(pieces_dir, name), "rb") as f:
        hdr = f.read(44)
        m, bnd, sh, firstRun, rc, ww, zero = struct.unpack("<IQIQQIQ", hdr)
        assert m == 0x31435250, "bad PRC1 magic"
        assert bnd == band and sh == shard and ww == w and zero == 0
        chars = f.read(rc)
        assert len(chars) == rc
        lens = f.read(4 * rc)
        assert len(lens) == 4 * rc
        words = (w * rc + 63) // 64
        hp = f.read(8 * words)
        tp = f.read(8 * words)
        assert len(hp) == len(tp) == 8 * words, f"piece {name} truncated"
        heads = []
        res, avail = 0, 0
        wi = 0
        words_buf = struct.unpack(f"<{words}Q", hp)
        for i in range(rc):
            while avail < w:
                res |= words_buf[wi] << avail
                wi += 1
                avail += 64
            heads.append(res & ((1 << w) - 1))
            res >>= w
            avail -= w
        tails = []
        res, avail = 0, 0
        wi = 0
        words_buf = struct.unpack(f"<{words}Q", tp)
        for i in range(rc):
            while avail < w:
                res |= words_buf[wi] << avail
                wi += 1
                avail += 64
            tails.append(res & ((1 << w) - 1))
            res >>= w
            avail -= w
        bands.setdefault(band, []).append((shard, firstRun, rc, chars, lens, heads, tails))
    nfiles += 1

nbands = (R + B - 1) // B
assert set(bands.keys()) == set(range(nbands)), f"band set mismatch: {len(bands)} bands vs {nbands}"

# Reassemble global columns in run order, asserting exact coverage.
heads_out, tails_out = [], []
cursor = 0
for band in range(nbands):
    subs = sorted(bands[band], key=lambda t: t[0])
    for shard, firstRun, rc, chars, lens, heads, tails in subs:
        assert firstRun == cursor, f"band {band} shard {shard}: firstRun {firstRun} != cursor {cursor}"
        heads_out.extend(heads)
        tails_out.extend(tails)
        cursor += rc
assert cursor == R, f"coverage {cursor} != R {R}"
print(f"pieces={nfiles} bands={nbands} runs={cursor} w={w} COVERAGE_EXACT")

# Compare against the banked dense columns (8-byte header + R x u64 LE).
def load_u64_col(path, expect):
    with open(path, "rb") as f:
        hdr = f.read(8)
        assert struct.unpack("<Q", hdr)[0] == expect, f"{path} header"
        data = f.read(8 * expect)
        assert len(data) == 8 * expect, f"{path} size"
        return list(struct.unpack(f"<{expect}Q", data))

ref_heads = load_u64_col(bank + ".ssa", R)
ref_tails = load_u64_col(bank + ".ssa_t", R)
assert heads_out == ref_heads, "HEADS_MISMATCH"
assert tails_out == ref_tails, "TAILS_MISMATCH"
print("HEADS_IDENTICAL")
print("TAILS_IDENTICAL")
print("PIECES_GATE_GREEN")
