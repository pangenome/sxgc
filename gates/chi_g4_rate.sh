#!/usr/bin/env bash
# G4 RATE - positions/s on the rehearsal container at --rss-gb 8, WARM, with
# the per-layer p50 costs, against the 13.7 positions/s locate-bound baseline.
#
#   usage: chi_g4_rate.sh XSA CONTAINER CORPUS WORKDIR [JOBS] [WINDOWS]
#
# The rate is the SECOND of two runs over the same window set (the first
# warms the arena and the page cache; both are printed). The forced mass
# (width1 + tforced + forced + rawfork) is the chi-proportional zero-locate
# mass; its per-layer p50 is expected in the microseconds-to-milliseconds
# regime, and the aggregate rate >= 100x the 13.7/s baseline.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
XSA=${1:?new xsa}
CONTAINER=${2:?rehearsal container}
CORPUS=${3:?rehearsal corpus}
WORK=${4:?work dir}
JOBS=${5:-16}
WINDOWS=${6:-/tmp/r100w.jsonl}
PATIENCE=${PATIENCE:-21600}
BASELINE=${BASELINE:-13.7}
mkdir -p "$WORK"

fail() { echo "GATE_FAIL: $*"; exit 1; }
[ -f "$WINDOWS" ] || fail "window set $WINDOWS is missing"

run() { # tag out
  local tag=$1 out=$2
  rm -f "$out" "$out.journal"
  setsid timeout --kill-after=10 "$PATIENCE" \
    "$XSA" consult --container "$CONTAINER" --corpus "$CORPUS" \
      --windows "$WINDOWS" --output "$out" \
      --rss-gb 8 -j "$JOBS" > "$WORK/$tag.log" 2>&1
  local rc=$?
  pgrep -f "xsa consult --container $CONTAINER" > /dev/null 2>&1 && fail "SURVIVOR consult process"
  [ $rc -eq 0 ] || { tail -5 "$WORK/$tag.log"; fail "run $tag exited $rc"; }
}

echo "=== G4: run 1 (cold) then run 2 (warm), --rss-gb 8, -j $JOBS"
run cold "$WORK/cold.rows"
run warm "$WORK/warm.rows"
cmp -s "$WORK/cold.rows" "$WORK/warm.rows" \
  && echo "G4_DETERMINISM two_runs_identical=1" \
  || fail "the two runs differ (determinism broken)"

for tag in cold warm; do
  DONE=$(grep -E "CONSULT_DONE" "$WORK/$tag.rows.journal" | tail -1)
  echo "G4_DONE_$tag $DONE"
done
WARM=$(grep -E "CONSULT_DONE" "$WORK/warm.rows.journal" | tail -1)
PPS=$(sed -n 's/.*positions_per_s=\([0-9.]*\).*/\1/p' <<<"$WARM" | head -1)
SECS=$(sed -n 's/.*seconds=\([0-9.]*\).*/\1/p' <<<"$WARM" | head -1)
POS=$(sed -n 's/.*positions=\([0-9]*\).*/\1/p' <<<"$WARM" | head -1)
[ -n "$PPS" ] || fail "no CONSULT_DONE receipt"

echo "=== G4: the per-layer receipts of the warm run"
grep -E "CONSULT_SEPFRAMING" "$WORK/warm.rows.journal" | tail -1
grep -E "CHI_ZEROLOCATE" "$WORK/warm.rows.journal" | tail -1
LAY=$(grep CONSULT_LAYERS "$WORK/warm.rows.journal" | tail -1)
[ -n "$LAY" ] || fail "no CONSULT_LAYERS receipt"
echo "G4_LAYERS $LAY"
PEAK=$(sed -n 's/.*peak_rss_gb=\([0-9.]*\).*/\1/p' <<<"$WARM" | head -1)

MULT=$(python3 -c "print('%.1f' % ($PPS / $BASELINE))")
echo "G4_RATE positions=$POS seconds=$SECS positions_per_s=$PPS baseline=$BASELINE multiple=${MULT}x peak_rss_gb=$PEAK cap_gb=8 jobs=$JOBS"
python3 - "$PPS" "$BASELINE" <<'EOF'
import sys
pps = float(sys.argv[1]); base = float(sys.argv[2])
assert pps >= 100 * base, 'the rate %.1f/s is below 100x the %.1f/s baseline' % (pps, base)
print('G4_RATE_OK positions_per_s=%.1f multiple=%.1fx baseline=%.1f' % (pps, pps / base, base))
EOF
[ $? -eq 0 ] || fail "the rate is below 100x the baseline"
echo "G4_RATE_GATE_OK"
