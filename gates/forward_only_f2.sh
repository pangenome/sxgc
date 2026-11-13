#!/usr/bin/env bash
# F2 - THE CURRENT CONTAINER STILL WORKS (forward-only gate).
#
# Runs the whole query surface against the ONE supported shape and writes every
# output plus a sha256 manifest, so two binaries (the pre-change one and the
# forward-only one) can be compared byte for byte.
#
#   usage: gates/forward_only_f2.sh BINARY CONTAINER CORPUS OUTDIR [JOBS]
#
# The comparison itself is gates/forward_only_f2_cmp.sh (it diffs the manifests
# and then the files).
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
X=${1:?binary}
CONTAINER=${2:?container}
CORPUS=${3:?corpus}
D=${4:?outdir}
J=${5:-32}

mkdir -p "$D"
echo "F2_BOX $(uptime)"
echo "F2_BINARY $X $(sha256sum "$X" | cut -d' ' -f1)"
echo "F2_CONTAINER $(ls -l "$CONTAINER")"
echo "F2_CORPUS $(ls -l "$CORPUS")"

# ---- the 200-window sets --------------------------------------------------
# consult windows: doc ids of the first 200 documents that are >= 4096 bytes,
# base resolved from the container's own m15 (BASE_ABSENT = the field omitted),
# start=1024 len=16 behind a 1024-byte left window -- the shape CONSULT.md §3
# documents and gates/no_cross_x3_consult.sh arm 1 uses.
python3 - "$CORPUS" "$D" <<'EOF'
import sys
corpus, d = sys.argv[1], sys.argv[2]
# stream the corpus once, only as far as the 200th document of >= 4096 bytes
starts, docs = [0], []
with open(corpus, 'rb') as f:
    buf = b''
    base = 0
    want = 200
    while len(docs) < want:
        chunk = f.read(1 << 22)
        if not chunk:
            break
        buf += chunk
        while True:
            j = buf.find(b'\x1e', 0)
            if j < 0:
                break
            nxt = base + j + 1
            starts.append(nxt)
            if nxt - starts[-2] >= 4096:
                docs.append(len(starts) - 2)
                if len(docs) == want:
                    break
            base += j + 1
            buf = buf[j + 1:]
        base += 0
with open(d + '/w200.jsonl', 'w') as f:
    for i in docs:
        f.write('{"doc": %d, "start": 1024, "len": 16, "left": 1024}\n' % i)
print('F2_WINDOWS docs=%d first=%s' % (len(docs), docs[:5]))
EOF
# docstarts probe positions: 200 pseudo-random u64 offsets inside the text
python3 - "$D" <<'EOF'
import struct, sys
d = sys.argv[1]
with open(d + '/pos200.bin', 'wb') as f:
    for i in range(200):
        f.write(struct.pack('<Q', 1234567 * (i + 1) + 99))
EOF

# a 200-byte query text for `xsa seeds`: the corpus's own bytes from a doc start
dd if="$CORPUS" of="$D/query200.txt" bs=1 skip=4096 count=200 status=none

# ---- the commands ---------------------------------------------------------
run() {
  name=$1
  shift
  echo "F2_RUN $name"
  setsid timeout --kill-after=10 3600 "$@" > "$D/$name.out" 2> "$D/$name.err"
  rc=$?
  echo "F2_RC name=$name rc=$rc bytes=$(stat -c%s "$D/$name.out")"
  if [ $rc -ne 0 ]; then
    echo "F2_FAIL name=$name rc=$rc"
    tail -5 "$D/$name.err"
    exit 1
  fi
}

# 1. interval widths only (no locate walks): the corpus' most frequent pattern
run query_count  "$X" query "$CONTAINER" "the " --count -j "$J"
# 2. the no-cross pattern search, corpus-certified
run search       "$X" search "$CONTAINER" --pattern "Harper Reed" --corpus "$CORPUS"
run search_jsonl "$X" search "$CONTAINER" --pattern "Harper Reed" --corpus "$CORPUS" --jsonl
# 3. consult over 200 windows (boundary-exact: the m15 + corpus arm)
run consult      "$X" consult --container "$CONTAINER" --windows "$D/w200.jsonl" \
                   --corpus "$CORPUS" --output "$D/consult.rows" -j "$J"
# 4. branch-of: the next-byte histogram after a context
run branch_of    "$X" branch-of "$CONTAINER" "the "
# 5. seeds on a 200-byte query
run seeds        "$X" seeds "$CONTAINER" "$D/query200.txt" --docs "$(dirname "$CORPUS")"
# 6. the m15 probe: rank/select of 200 positions
run docstarts_probe "$X" docstarts probe "$CONTAINER" --positions "$D/pos200.bin"
run docstarts_info  "$X" docstarts info "$CONTAINER"

# ---- the manifest ---------------------------------------------------------
( cd "$D" && sha256sum consult.rows query_count.out search.out search_jsonl.out \
    branch_of.out seeds.out docstarts_probe.out docstarts_info.out > MANIFEST.sha256 )
echo "F2_MANIFEST $D/MANIFEST.sha256"
cat "$D/MANIFEST.sha256"
echo "F2_DONE"
