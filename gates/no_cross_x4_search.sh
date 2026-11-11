#!/usr/bin/env bash
# X4 - SEARCH: crossing matches are excluded by default; --cross-doc returns
# them; the excluded set must be EXACTLY {rank(s) != rank(e-1)}.
#
#   >=200 sampled query patterns with matches, run through `xsa search` in
#   both arms over the X2 container (member m15):
#     - default:   crossing matches absent, every kept row cross=0
#     - --cross-doc: every match present, crossing ones marked cross=1
#   verified against an independent starts-table classification (seps.bin
#   parsed in plain python, no shared code): a match [pos, pos+len) crosses
#   iff a document start lies inside (pos, pos+len-1].
#
# usage: gates/no_cross_x4_search.sh CONTAINER CORPUS SEPS_BIN WORKDIR
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
X=${X:-$(cd "$HERE/.." && pwd)/xsa/target/release/xsa}
CONTAINER=${1:?container}
CORPUS=${2:?corpus}
SEPS=${3:?seps.bin}
D=${4:?workdir}
mkdir -p "$D"
echo "X4_BOX $(uptime)"

python3 - "$CORPUS" "$SEPS" "$D" <<'EOF'
import struct, sys, random
corpus, seps, d = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(seps, 'rb').read()
docs = struct.unpack_from('<Q', data, 16)[0]
starts = [0]
at = 0
for i in range(1, docs):
    at += struct.unpack_from('<I', data, 48 + 4 * i)[0]
    starts.append(at)
text = open(corpus, 'rb')
import os
flen = os.path.getsize(corpus)
rng = random.Random(4242)
def read_at(off, n):
    text.seek(off)
    return text.read(n)
pats = []
tries = 0
# 130 unique-ish 48-byte patterns from random positions
while len(pats) < 130 and tries < 2000:
    tries += 1
    off = rng.randrange(0, flen - 64)
    p = read_at(off, 48)
    if b'\n' in p:
        continue
    pats.append(p)
# 70 sixteen-byte patterns sampled from near document starts, where
# multi-occurrence and boundary-crossing matches concentrate (16 bytes keeps
# the locate cost sane; 8-byte Pile contexts can occur 1e6+ times)
# plus 40 twenty-four-byte patterns, so the arm has >= 200 WITH OUTPUT even
# if a few are refused by --max-occ
tries = 0
while len(pats) < 200 and tries < 4000:
    tries += 1
    i = rng.randrange(0, len(starts) - 1)
    off = starts[i] + rng.randrange(0, 64)
    p = read_at(off, 16)
    if b'\n' in p:
        continue
    pats.append(p)
tries = 0
while len(pats) < 200 and tries < 4000:
    tries += 1
    off = rng.randrange(0, flen - 64)
    p = read_at(off, 24)
    if b'\n' in p:
        continue
    pats.append(p)
# 120 STRADDLING patterns: 24 bytes centred on a document start, so the match
# at the sampled position CROSSES by construction - the excluded set must be
# exactly these (minus any other non-crossing occurrences of the same bytes)
straddle = 0
tries = 0
i = 40
while straddle < 120 and tries < 1000:
    tries += 1
    i += 7
    if i >= len(starts) - 1:
        break
    off = starts[i] - 12
    if off < 0:
        continue
    p = read_at(off, 24)
    if b'\n' in p:
        continue
    pats.append(p)
    straddle += 1
with open(d + '/patterns.hex', 'w') as f:
    for p in pats:
        f.write(p.hex() + '\n')
with open(d + '/starts.bin', 'wb') as f:
    for s in starts:
        f.write(struct.pack('<Q', s))
print("X4_PATTERNS sampled=%d" % len(pats))
EOF
N=$(wc -l < "$D/patterns.hex")

"$X" search "$CONTAINER" --patterns "$D/patterns.hex" --hex \
     --corpus "$CORPUS" --max-occ 8192 > "$D/default.tsv" 2> "$D/default.err"
rc=$?
[ $rc -eq 0 ] || { echo "X3_FAIL search default exited $rc"; cat "$D/default.err"; exit 1; }
grep "SEARCH_BOUNDARY" "$D/default.err" | tail -1

"$X" search "$CONTAINER" --patterns "$D/patterns.hex" --hex --cross-doc \
     --corpus "$CORPUS" --max-occ 8192 > "$D/cross.tsv" 2> "$D/cross.err"
rc=$?
[ $rc -eq 0 ] || { echo "X4_FAIL search --cross-doc exited $rc"; cat "$D/cross.err"; exit 1; }
grep "SEARCH_BOUNDARY" "$D/cross.err" | tail -1

python3 - "$D" <<'EOF'
import struct, sys, bisect
d = sys.argv[1]
raw = open(d + '/starts.bin', 'rb').read()
starts = [struct.unpack_from('<Q', raw, i)[0] for i in range(0, len(raw), 8)]

def base_of(p):
    return starts[bisect.bisect_right(starts, p) - 1]

def parse(path):
    rows = {}
    for ln in open(path):
        f = ln.rstrip('\n').split('\t')
        if len(f) != 4:
            continue
        i, pos, ln2, cross = int(f[0]), int(f[1]), int(f[2]), int(f[3])
        rows.setdefault((i, pos, ln2), cross)
    return rows

default = parse(d + '/default.tsv')
cross = parse(d + '/cross.tsv')
fails = 0

# 1. the default output has no crossing match and no cross=1 row
for key, c in default.items():
    if c != 0:
        fails += 1
        print("X4_FAIL default row %r carries cross=1" % (key,))
        break
# 2. the excluded set is exactly {rank(s) != rank(e-1)}
bad_cls = 0
excluded = []
for (i, pos, ln), c in cross.items():
    crossing = base_of(pos + ln - 1) > pos
    if c != (1 if crossing else 0):
        bad_cls += 1
        if bad_cls == 1:
            print("X4_FAIL classification mismatch at id=%d pos=%d len=%d flag=%d want=%d"
                  % (i, pos, ln, c, 1 if crossing else 0))
    if crossing:
        excluded.append((i, pos, ln))
if bad_cls:
    fails += 1
# 3. every default row is a non-crossing row of the cross arm, and every
#    non-crossing cross-arm row is in the default output
missing = [k for k in default if k not in cross]
if missing:
    fails += 1
    print("X4_FAIL %d default rows are absent from the --cross-doc arm" % len(missing))
kept_cross = {k for k, c in cross.items() if c == 0}
lost = kept_cross - set(default)
if lost:
    fails += 1
    print("X4_FAIL %d non-crossing matches are missing from the default output" % len(lost))
# 4. the default arm excluded exactly the crossing matches
gone = set(cross) - set(default)
if gone != set(excluded):
    fails += 1
    print("X4_FAIL excluded set != crossing set (%d vs %d)"
          % (len(gone), len(excluded)))
patterns = len({k[0] for k in cross})
print("X4_EXCLUDED_SET patterns_with_output=%d default_rows=%d cross_rows=%d crossing_excluded=%d exact=1%s"
      % (patterns, len(default), len(cross), len(excluded),
         '' if fails == 0 else ' (FAILED)'))
print("X4_RESULT %s" % ('PASS' if fails == 0 and patterns >= 200 else 'FAIL'))
sys.exit(1 if fails or patterns < 200 else 0)
EOF
rc=$?
exit $rc
