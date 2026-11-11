#!/usr/bin/env bash
# X3 - END-TO-END: `xsa consult` on the X2 container over >=10,000 windows.
#
#   arm 1  12,500 windows (doc, start=1024, len=4, left=1024) in documents of
#          >= 4096 bytes: 50,000 histograms, every one must be boundary-exact
#          (flag bit 4 CLEAR) - no conflated row is tolerated in this arm
#   arm 2  2,500 RAMP windows (start=1, len=16): the real zone-(c) incidence
#          on Pile text, reported (the trie layer covers ctx <= 8; 9..16-byte
#          contexts past --boundary-cap fall back MARKED)
#   arm 3  --exclude-source over arm 1: width(on) == width(off) - 1 on every
#          row, the own byte's pair decremented, every other pair unchanged
#   arm 4  >=50 positions brute-forced by gates/nocross_oracle.c (memmem over
#          the whole corpus, starts-table-aware, no index, no shared code)
#   arm 5  docid/base/depth of every arm-1 row against the map
#
# usage: gates/no_cross_x3_consult.sh CONTAINER CORPUS SEPS_BIN WORKDIR [JOBS]
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
X=${X:-$(cd "$HERE/.." && pwd)/xsa/target/release/xsa}
CONTAINER=${1:?container}
CORPUS=${2:?corpus}
SEPS=${3:?seps.bin}
D=${4:?workdir}
J=${5:-32}
ORACLE=${ORACLE:-/tmp/nxc/nocross_oracle}

mkdir -p "$D"
[ -x "$ORACLE" ] || { gcc -O2 -o "$ORACLE" "$HERE/nocross_oracle.c"; }

echo "X3_BOX $(uptime)"
echo "X3_CONTAINER $(ls -l "$CONTAINER")"

# ---- the window sets --------------------------------------------------------
python3 - "$SEPS" "$D" <<'EOF'
import struct, sys, random
seps, d = sys.argv[1], sys.argv[2]
data = open(seps, 'rb').read()
docs = struct.unpack_from('<Q', data, 16)[0]
starts = [0]
at = 0
for i in range(1, docs):
    at += struct.unpack_from('<I', data, 48 + 4 * i)[0]
    starts.append(at)
# arm 1: full-context windows in docs >= 4096 B
w1 = []
for i, s in enumerate(starts):
    e = starts[i + 1] if i + 1 < len(starts) else None
    if e is None or e - s >= 4096:
        w1.append(i)
    if len(w1) >= 12500:
        break
assert len(w1) == 12500, len(w1)
with open(d + '/w1.jsonl', 'w') as f:
    for i in w1:
        f.write('{"doc": %d, "start": 1024, "len": 4, "left": 1024}\n' % i)
# arm 2: the short-context ramp, where the enumeration cost concentrates.
# 100 windows at the DEFAULT cap (8192): the true default-cap incidence and
# wall. The measured locate is ~13 ms at 10 GB (12.6 us per block decode,
# ~512-run LF walks), so a width-8000 row costs ~100 s: this arm is small ON
# PURPOSE and its wall is part of the receipt.
with open(d + '/w2.jsonl', 'w') as f:
    for i in w1[:100]:
        f.write('{"doc": %d, "start": 1, "len": 16, "left": 1024}\n' % i)
# arm 2b: the same ramp geometry at --boundary-cap 256 (the cost knob): the
# marked fraction at the cheap setting, for the cap tradeoff the report states
with open(d + '/w2b.jsonl', 'w') as f:
    for i in w1[100:600]:
        f.write('{"doc": %d, "start": 1, "len": 16, "left": 1024}\n' % i)
# arm 4: 60 oracle positions drawn from ARM 1's OWN rows (depths 1024..1027),
# spread over the window docs
rng = random.Random(20261111)
with open(d + '/oracle-q.txt', 'w') as f:
    for i in rng.sample(w1, 60):
        depth = 1024 + rng.randrange(4)
        f.write('%d %d\n' % (starts[i] + depth, 1024))
print("X3_WINDOWS full_context=12500 ramp=500 oracle=60")
EOF

run() { # tag windows extra...
  local tag=$1 win=$2 out=$3; shift 3
  rm -f "$out" "$out.journal"
  "$X" consult --container "$CONTAINER" --corpus "$CORPUS" \
      --windows "$win" --output "$out" --rss-gb 8 -j "$J" "$@"
  local rc=$?
  [ $rc -eq 0 ] || { echo "X3_FAIL consult $tag exited $rc"; tail -3 "$out.journal" 2>/dev/null; exit 1; }
}

run arm1 "$D/w1.jsonl" "$D/arm1.rows"
run arm2 "$D/w2.jsonl" "$D/arm2.rows"
run arm2b "$D/w2b.jsonl" "$D/arm2b.rows" --boundary-cap 256
run arm3 "$D/w1.jsonl" "$D/arm3.rows" --exclude-source

grep -E "BOUNDARY_INEXACT|CONSULT_DONE" "$D/arm1.rows.journal" | tail -2
grep -E "BOUNDARY_INEXACT" "$D/arm2.rows.journal" | tail -1
grep -E "BOUNDARY_INEXACT" "$D/arm2b.rows.journal" | tail -1

python3 - "$SEPS" "$D" "$CORPUS" <<'EOF'
import struct, sys, random, bisect
seps, d, corpus_path = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(seps, 'rb').read()
docs = struct.unpack_from('<Q', data, 16)[0]
starts = [0]
at = 0
for i in range(1, docs):
    at += struct.unpack_from('<I', data, 48 + 4 * i)[0]
    starts.append(at)

def read_rows(path):
    f = open(path, 'rb').read()
    assert f[:8] == b'XSACONS1', 'bad magic'
    ver = struct.unpack_from('<I', f, 8)[0]
    assert ver == 3, 'row file version %d' % ver
    rows = []
    p = 16
    while p < len(f):
        nsym, ctxlen, flags = struct.unpack_from('<IHH', f, p)
        doc, off, width, eof = struct.unpack_from('<QQQQ', f, p + 8)
        pairs = []
        q = p + 40
        for _ in range(nsym):
            pairs.append((f[q], struct.unpack_from('<Q', f, q + 1)[0])); q += 9
        docid, base, depth = struct.unpack_from('<QQQ', f, q)
        rows.append(dict(flags=flags, off=off, width=width, eof=eof, pairs=pairs,
                         docid=docid, base=base, depth=depth, ctxlen=ctxlen))
        p = q + 24
    return rows

fails = 0
# ---- arm 1: every histogram boundary-exact, the width identity, docid/base/depth
a1 = read_rows(d + '/arm1.rows')
print("X3_ARM1 rows=%d" % len(a1))
marked = [r for r in a1 if r['flags'] & 16]
if marked:
    fails += 1
    print("X3_FAIL %d arm-1 rows are boundary_exact=0 (first: %r)" % (len(marked), marked[0]))
else:
    print("X3_ARM1_EXACT boundary_exact=1 rows=%d marked=0" % len(a1))
bad_ident = [r for r in a1 if r['width'] != sum(c for _, c in r['pairs']) + r['eof']]
if bad_ident:
    fails += 1
    print("X3_FAIL %d arm-1 rows break width == sum(pairs)+eof" % len(bad_ident))
bad_doc = 0
for r in a1:
    i = bisect.bisect_right(starts, r['off']) - 1
    if r['docid'] != i or r['base'] != starts[i] or r['depth'] != r['off'] - starts[i]:
        bad_doc += 1
        if bad_doc == 1:
            print("X3_FAIL first docid/base/depth mismatch at off=%d: %d/%d/%d want %d/%d/%d"
                  % (r['off'], r['docid'], r['base'], r['depth'], i, starts[i], r['off'] - starts[i]))
print("X3_DOCID_BASE_DEPTH rows=%d mismatches=%d" % (len(a1), bad_doc))
fails += bad_doc

# ---- arm 2: the zone-(c) incidence on ramp windows, at the default cap
a2 = read_rows(d + '/arm2.rows')
m2 = sum(1 for r in a2 if r['flags'] & 16)
short = sum(1 for r in a2 if r['ctxlen'] <= 8)
print("X3_RAMP rows=%d ctxlen<=8=%d marked_inexact=%d ratio=%.6f cap=8192-default"
      % (len(a2), short, m2, m2 / max(1, len(a2))))
if m2 > len(a2) // 100:
    print("X3_RAMP_LOUD marked rows exceed 1%% of the ramp arm - the trie depth / cap defaults need revisiting")
else:
    print("X3_RAMP_INCIDENCE below_1pct=1")
# ---- arm 2b: the same geometry at --boundary-cap 256 (the cost knob)
a2b = read_rows(d + '/arm2b.rows')
m2b = sum(1 for r in a2b if r['flags'] & 16)
print("X3_RAMP_CAP256 rows=%d marked_inexact=%d ratio=%.6f note=cost-knob arm"
      % (len(a2b), m2b, m2b / max(1, len(a2b))))

# ---- arm 3: --exclude-source is width-1, own byte decremented, others unchanged
a3 = read_rows(d + '/arm3.rows')
CORPUS = sys.argv[3]
text = open(CORPUS, 'rb')
if len(a3) != len(a1):
    fails += 1
    print("X3_FAIL arm3 rows %d != arm1 rows %d" % (len(a3), len(a1)))
else:
    bad = 0
    excluded = 0
    for x, y in zip(a1, a3):
        if x['off'] != y['off']:
            bad += 1; break
        if not (y['flags'] & 4):
            bad += 1; break          # every arm-1 position is inside the text
        excluded += 1
        if y['width'] != x['width'] - 1 or y['eof'] != x['eof']:
            bad += 1; break
        text.seek(x['off'])
        own = text.read(1)[0]
        dx, dy = dict(x['pairs']), dict(y['pairs'])
        for b in set(dx) | set(dy):
            want = dx.get(b, 0) - (1 if b == own else 0)
            if dy.get(b, 0) != want:
                bad += 1
                print("X3_FAIL exclude pair mismatch at off=%d byte=%d %d->%d want %d"
                      % (x['off'], b, dx.get(b, 0), dy.get(b, 0), want))
                break
    print("X3_EXCLUDE rows=%d excluded=%d result=%s"
          % (len(a3), excluded, 'PASS' if bad == 0 else 'FAIL'))
    if bad:
        fails += 1
print("X3_PY_FAILS %d" % fails)
sys.exit(1 if fails else 0)
EOF
rc=$?

# ---- arm 4: the brute-force oracle spot checks ------------------------------
"$ORACLE" "$SEPS" "$CORPUS" $(stat -c%s "$CORPUS" 2>/dev/null || echo 10000000000) "$D/oracle-q.txt" > "$D/oracle.out"
python3 - "$D" <<'EOF'
import struct, sys
d = sys.argv[1]
def read_rows(path):
    f = open(path, 'rb').read()
    rows = {}
    p = 16
    while p < len(f):
        nsym, ctxlen, flags = struct.unpack_from('<IHH', f, p)
        doc, off, width, eof = struct.unpack_from('<QQQQ', f, p + 8)
        pairs = []
        q = p + 40
        for _ in range(nsym):
            pairs.append((f[q], struct.unpack_from('<Q', f, q + 1)[0])); q += 9
        rows[off] = (width, eof, pairs)
        p = q + 24
    return rows
rows = read_rows(d + '/arm1.rows')
bad = 0
checked = 0
for ln in open(d + '/oracle.out'):
    f = dict(kv.split('=') for kv in ln.split() if '=' in kv)
    off = int(f['off'])
    want_w, want_e = int(f['width']), int(f['eof'])
    want_p = []
    if f['pairs']:
        for kv in f['pairs'].split(','):
            b, c = kv.split(':')
            want_p.append((int(b), int(c)))
    got_w, got_e, got_p = rows[off]
    ok = got_w == want_w and got_e == want_e and got_p == want_p
    checked += 1
    if not ok:
        bad += 1
        print("X3_ORACLE_MISMATCH off=%d got w=%d e=%d %r want w=%d e=%d %r"
              % (off, got_w, got_e, got_p, want_w, want_e, want_p))
print("X3_ORACLE_SPOT_CHECK positions=%d exact=%d mismatches=%d result=%s"
      % (checked, checked - bad, bad, 'PASS' if bad == 0 and checked >= 50 else 'FAIL'))
sys.exit(1 if bad or checked < 50 else 0)
EOF
rc2=$?

total=$((rc + rc2))
echo "X3_RESULT $([ $total -eq 0 ] && echo PASS || echo FAIL)"
exit $total
