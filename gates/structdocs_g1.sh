#!/usr/bin/env bash
# Gate G1 for the structural-docs lane: boundary parity of the container's
# document-start member (m15) against the authoritative seps.bin map, with the
# positions inside internal-0x1E documents deliberately included, plus a
# byte-for-byte diff against the INDEPENDENT C oracle (gates/docstarts_oracle.c).
#
#   usage: structdocs_g1.sh XSA CONTAINER MAPDIR WORKDIR [SAMPLES] [RANGE_END]
#
# SAMPLES defaults to 10000 (the gate's spec); RANGE_END defaults to the
# container's whole text. Exit 0 only on zero disagreements AND an exact
# probe/oracle diff.
set -uo pipefail
XSA=${1:?xsa binary}
CONTAINER=${2:?container with member m15}
MAPDIR=${3:?map dir (seps.bin [+meta.bin])}
WORK=${4:?work dir}
SAMPLES=${5:-10000}
RANGE_END=${6:-}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$WORK"

fail() { echo "G1_FAIL: $*"; exit 1; }

# ---- the member-vs-map parity instrument (its own third decoder inside)
if [ -n "$RANGE_END" ]; then
  "$XSA" docstarts verify "$CONTAINER" --docs-map "$MAPDIR" \
      --samples "$SAMPLES" --range-end "$RANGE_END" | tee "$WORK/g1-verify.log"
else
  "$XSA" docstarts verify "$CONTAINER" --docs-map "$MAPDIR" \
      --samples "$SAMPLES" | tee "$WORK/g1-verify.log"
fi
grep -q "DOCSTARTS_PARITY result=PASS" "$WORK/g1-verify.log" \
  || fail "the member/map parity instrument did not PASS"
grep -q "disagreements=0 " "$WORK/g1-verify.log" \
  || fail "disagreements must be 0"
echo "G1_PARITY member_vs_map disagreements=0 result=PASS"

# ---- the independent C oracle: same positions, byte-for-byte PROBE lines
ORACLE="$WORK/docstarts-oracle"
gcc -O3 -march=native -o "$ORACLE" "$HERE/docstarts_oracle.c" || fail "oracle build"

python3 - "$MAPDIR" "$WORK" "$SAMPLES" "$CONTAINER" <<'EOF'
import json, os, random, struct, sys
mapdir, work, samples, container = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
b = open(os.path.join(mapdir, 'seps.bin'), 'rb').read()
docs = struct.unpack_from('<Q', b, 16)[0]
corpus = struct.unpack_from('<Q', b, 24)[0]
stride = struct.unpack_from('<I', b, 12)[0]
deltas_off, anchors_off = 48, 48 + 4 * docs
# THE TEXT IS THE CONTAINER'S TEXT (n-1), not the map's corpus: a prefix
# container resolves only the documents that START inside its text
ch = open(container, 'rb').read(64)
text_len = struct.unpack_from('<Q', ch, 8)[0] - 1
max_doc = 0
while max_doc < docs:
    # doc starts are ascending: bisect the doc id whose start reaches the text
    lo, hi = 0, docs
    while lo < hi:
        mid = (lo + hi) // 2
        j = mid // stride
        s = struct.unpack_from('<Q', b, anchors_off + 8 * j)[0]
        d = j * stride
        while d < mid:
            d += 1
            s += struct.unpack_from('<I', b, deltas_off + 4 * d)[0]
        if s < text_len:
            lo = mid + 1
        else:
            hi = mid
    max_doc = lo
    break
max_doc = max(0, max_doc - 1)
def select(i):
    j = i // stride
    s = struct.unpack_from('<Q', b, anchors_off + 8 * j)[0]
    d = j * stride
    while d < i:
        d += 1
        s += struct.unpack_from('<I', b, deltas_off + 4 * d)[0]
    return s
# positions: random doc starts (exercising select) plus mid-document positions
rng = random.Random(20261111)
starts = [select(rng.randrange(max_doc)) for _ in range(samples // 2)]
mids = [select(rng.randrange(max_doc)) + rng.randrange(1, 512) for _ in range(samples // 2)]
pos = [p for p in starts + mids if p < text_len]
# every internal-0x1E document in meta.bin gets a position inside it
meta = os.path.join(mapdir, 'meta.bin')
internal = 0
if os.path.exists(meta):
    m = open(meta, 'rb').read()
    mdocs = struct.unpack_from('<Q', m, 8)[0]
    shard_names = struct.unpack_from('<Q', m, 24)[0]
    names = struct.unpack_from('<Q', m, 32)[0]
    name_payload = struct.unpack_from('<Q', m, 40)[0]
    shard_payload = struct.unpack_from('<Q', m, 48)[0]
    rows_off = 68 + name_payload + 6 * names + shard_payload + 6 * shard_names
    for d in range(mdocs):
        row = rows_off + 34 * d
        off = struct.unpack_from('<Q', m, row)[0]
        if off >= text_len:
            break
        if struct.unpack_from('<I', m, row + 30)[0]:
            ln = struct.unpack_from('<Q', m, row + 8)[0]
            p = off + max(1, ln // 2)
            if p < text_len:
                pos.append(p)
                internal += 1
with open(os.path.join(work, 'g1-positions.bin'), 'wb') as f:
    for p in pos:
        f.write(struct.pack('<Q', p))
print('G1_ORACLE_POSITIONS random=%d internal_0x1e_docs=%d total=%d text_len=%d'
      % (samples, internal, len(pos), text_len))
EOF

"$XSA" docstarts probe "$CONTAINER" --positions "$WORK/g1-positions.bin" \
    > "$WORK/g1-probe-member.txt" || fail "probe run"
"$ORACLE" "$MAPDIR/seps.bin" "$WORK/g1-positions.bin" \
    > "$WORK/g1-probe-oracle.txt" || fail "oracle run"
if cmp -s "$WORK/g1-probe-member.txt" "$WORK/g1-probe-oracle.txt"; then
  echo "G1_ORACLE_DIFF member_vs_c_oracle identical=1 lines=$(wc -l < "$WORK/g1-probe-member.txt") result=PASS"
else
  fail "the member probe and the independent C oracle disagree"
fi
echo "G1_RESULT PASS"
