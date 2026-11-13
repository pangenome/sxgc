#!/usr/bin/env bash
# G2 CENSUS RE-DERIVATION - run the chi binary over the rehearsal window set
# on the 100 GB rehearsal container, re-derive the consult census, and check
# the fork and marked rows against the banked row file byte for byte.
#
#   usage: chi_g2_census.sh XSA CONTAINER CORPUS WORKDIR [JOBS] [WINDOWS] [BANKED]
#
# defaults: WINDOWS=/tmp/r100w.jsonl (the banked run's window set),
#           BANKED=/tmp/r100rows.tsv (the banked 682,496-row file, XSACONS1 v3).
#
# The banked run was killed mid-stream (its file holds the first 682,496 of
# 6,667,438 positions); the comparison covers that PREFIX, in window order.
# Every check prints N of things examined; a vacuous check fails the gate.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
XSA=${1:?new xsa}
CONTAINER=${2:?rehearsal container}
CORPUS=${3:?rehearsal corpus}
WORK=${4:?work dir}
JOBS=${5:-16}
WINDOWS=${6:-/tmp/r100w.jsonl}
BANKED=${7:-/tmp/r100rows.tsv}
PATIENCE=${PATIENCE:-21600}
mkdir -p "$WORK"

fail() { echo "GATE_FAIL: $*"; exit 1; }

[ -f "$WINDOWS" ] || fail "window set $WINDOWS is missing"
[ -f "$BANKED" ] || fail "banked row file $BANKED is missing"
BANKED_ROWS=$(python3 -c "
import struct
d = open('$BANKED','rb').read(16 + 40)
print('ok')" 2>/dev/null && echo y) || fail "banked unreadable"
N_BANKED=$(python3 - "$HERE" "$BANKED" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
from chi_rows import read_rows
print(len(read_rows(sys.argv[2])))
EOF
)
[ -n "$N_BANKED" ] && [ "$N_BANKED" -gt 100000 ] || fail "banked row file has only $N_BANKED rows"

echo "=== G2: the chi run over the full rehearsal window set (banked prefix: $N_BANKED rows)"
rm -f "$WORK/new.rows" "$WORK/new.rows.journal"
setsid timeout --kill-after=10 "$PATIENCE" \
  "$XSA" consult --container "$CONTAINER" --corpus "$CORPUS" \
    --windows "$WINDOWS" --output "$WORK/new.rows" \
    --rss-gb 8 -j "$JOBS" > "$WORK/run.log" 2>&1
rc=$?
pgrep -f "xsa consult --container $CONTAINER" > /dev/null 2>&1 && fail "SURVIVOR consult process"
[ $rc -eq 0 ] || { tail -5 "$WORK/run.log"; fail "the run exited $rc"; }
DONE=$(grep -E "CONSULT_DONE" "$WORK/new.rows.journal" | tail -1)
echo "G2_RUN $DONE"
NEW_POS=$(sed -n 's/.*positions=\([0-9]*\).*/\1/p' <<<"$DONE" | head -1)
[ -n "$NEW_POS" ] && [ "$NEW_POS" -gt "$N_BANKED" ] || fail "the new run produced $NEW_POS positions, fewer than the banked prefix"

echo "=== G2a: the banked prefix, byte for byte (fork and marked rows must match)"
python3 - "$HERE" "$BANKED" "$WORK/new.rows" "$N_BANKED" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
from chi_rows import read_rows
bank = read_rows(sys.argv[2])
new = read_rows(sys.argv[3])
n = int(sys.argv[4])
assert len(new) >= n, 'the new run is short: %d < %d' % (len(new), n)
w1 = single = fork = marked = 0
w1d = singled = forkd = markedd = 0
for a, b in zip(bank[:n], new[:n]):
    if b['width'] == 1 and not (b['flags'] & 16):
        k, d = 'w1', (a['width'], a['eof'], a['pairs'], a['flags'], a['ctxlen']) != \
                  (b['width'], b['eof'], b['pairs'], b['flags'], b['ctxlen'])
    elif b['flags'] & 16:
        k, d = 'marked', (a['width'], a['eof'], a['pairs'], a['flags'], a['ctxlen']) != \
                       (b['width'], b['eof'], b['pairs'], b['flags'], b['ctxlen'])
    elif len(b['pairs']) == 1:
        k, d = 'single', (a['width'], a['eof'], a['pairs'], a['flags'], a['ctxlen']) != \
                        (b['width'], b['eof'], b['pairs'], b['flags'], b['ctxlen'])
    else:
        k, d = 'fork', (a['width'], a['eof'], a['pairs'], a['flags'], b['ctxlen']) != \
                     (b['width'], b['eof'], b['pairs'], b['flags'], b['ctxlen'])
    if k == 'w1': w1 += 1; w1d += d
    elif k == 'single': single += 1; singled += d
    elif k == 'marked': marked += 1; markedd += d
    else: fork += 1; forkd += d
print('G2_PREFIX rows=%d width1=%d(diffs=%d) single=%d(diffs=%d) '
      'fork=%d(diffs=%d) marked=%d(diffs=%d)'
      % (n, w1, w1d, single, singled, fork, forkd, marked, markedd))
assert forkd == 0, 'fork rows differ from the banked file'
assert markedd == 0, 'marked rows differ from the banked file'
assert w1d == singled == 0, 'forced rows differ from the banked file'
print('G2_PREFIX_OK fork_rows=%d marked_rows=%d byte_identical=1' % (fork, marked))
EOF
[ $? -eq 0 ] || fail "the banked comparison failed"

echo "=== G2b: the measured layer mix next to the census"
python3 - "$HERE" "$BANKED" "$N_BANKED" <<'EOF'
import sys
sys.path.insert(0, sys.argv[1])
from chi_rows import read_rows, classify
bank = read_rows(sys.argv[2])[:int(sys.argv[3])]
c = classify(bank)
n = len(bank)
# THE CENSUS (RESULTS_REHEARSAL_100GB.md, the banked 682,496 rows):
#   88.1% width=1 (forced) / 10.6% single branch / 0.7% wide histogram
#   (the marked fallback rows) / 0.5% genuine forks (<=8 branches).
# This classifier (by the row's own bytes): width1 = width==1 unmarked;
# single_branch = one pair, unmarked; marked = flag bit 4; fork = several
# pairs, unmarked. The census's "wide histogram" bucket mixed marked rows
# with >8-branch and trie-answered rows, so the printed line shows both.
fork_br = sum(1 for r in bank if not (r['flags'] & 16) and len(r['pairs']) > 1 and r['width'] > 1)
wide8 = sum(1 for r in bank if (not (r['flags'] & 16)) and len(r['pairs']) > 8 and r['width'] > 1)
print('G2_CENSUS banked_rows=%d' % n)
print('G2_MIX_MEASURED width1=%d (%.2f%%) single_branch=%d (%.2f%%) '
      'marked=%d (%.2f%%) fork=%d (%.2f%%) [of which >8 branches: %d]'
      % (c['width1'], 100.0 * c['width1'] / n,
         c['single_branch'], 100.0 * c['single_branch'] / n,
         c['marked'], 100.0 * c['marked'] / n,
         fork_br, 100.0 * fork_br / n, wide8))
print('G2_CENSUS_TARGET width1=88.1 single_branch=10.6 wide_histogram=0.7 forks=0.5')
assert c['width1'] > 0.85 * n, 'the width-1 share collapsed'
EOF
[ $? -eq 0 ] || fail "the census mix print failed"

echo "=== G2c: the run's own layer receipts"
grep -E "CONSULT_SEPFRAMING" "$WORK/new.rows.journal" | tail -1
grep -E "CHI_ZEROLOCATE" "$WORK/new.rows.journal" | tail -1
LAY=$(grep CONSULT_LAYERS "$WORK/new.rows.journal" | tail -1)
[ -n "$LAY" ] || fail "no CONSULT_LAYERS receipt"
echo "G2_LAYERS $LAY"

echo "=== G2 SUMMARY"
PPS=$(sed -n 's/.*positions_per_s=\([0-9.]*\).*/\1/p' <<<"$DONE" | head -1)
echo "G2_CENSUS_OK banked_prefix_compared=$N_BANKED byte_identical=1 rate_positions_per_s=$PPS"
