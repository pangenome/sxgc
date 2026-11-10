#!/usr/bin/env bash
# Gates A2 (determinism), A3 (bounded memory + throughput) and A4 (crash resume)
# for `xsa consult`.
#
#   usage: consult_a234.sh XSA CONTAINER CORPUS WORKDIR [JOBS]
#
# Every process launch is `setsid timeout --kill-after=10 N`, and every step
# asserts that no `xsa consult` process of ours survived it.
set -uo pipefail

XSA=${1:?xsa binary}
CONTAINER=${2:?container}
CORPUS=${3:?corpus}
WORK=${4:?work dir}
JOBS=${5:-96}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$WORK"

fail() { echo "GATE_FAIL: $*"; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }
field() { # file key
  sed -n 's/.*\b'"$2"'=\([^ ]*\).*/\1/p' "$1" | tail -1
}
no_survivors() {
  local pids
  pids=$(pgrep -f "xsa consult --container $CONTAINER" 2>/dev/null || true)
  if [ -n "$pids" ]; then
    for p in $pids; do
      tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -qF "$WORK" && \
        fail "SURVIVOR pid=$p $(tr '\0' ' ' < /proc/$p/cmdline)"
    done
  fi
  echo "NO_SURVIVORS work=$WORK"
}

run() { # tag windows rss_gb out extra...
  local tag=$1 win=$2 rss=$3 out=$4; shift 4
  rm -f "$out" "$out.journal"
  setsid timeout --kill-after=10 "${PATIENCE:-7200}" \
    "$XSA" consult --container "$CONTAINER" --corpus "$CORPUS" \
    --windows "$win" --output "$out" --rss-gb "$rss" -j "$JOBS" "$@"
  local rc=$?
  no_survivors
  [ $rc -eq 0 ] || fail "run $tag exited $rc"
}

python3 "$HERE/consult_windows.py" "$WORK/w1m.jsonl" --positions 1000000 --len 2048 \
  --offset 3000000512 | tee "$WORK/windows.log"
python3 "$HERE/consult_windows.py" "$WORK/w50k.jsonl" --positions 50000 --len 2048 \
  --offset 3000000512 | tee -a "$WORK/windows.log"

echo "=== A3/A2a: the 1M-position T2048 set at --rss-gb 8 (run 1 and run 2)"
run a3r1 "$WORK/w1m.jsonl" 8 "$WORK/a3r1.rows"
run a3r2 "$WORK/w1m.jsonl" 8 "$WORK/a3r2.rows"
S1=$(sha "$WORK/a3r1.rows"); S2=$(sha "$WORK/a3r2.rows")
ROWS=$(field "$WORK/a3r1.rows.journal" positions)
PPS=$(field "$WORK/a3r1.rows.journal" positions_per_s)
PEAK=$(field "$WORK/a3r1.rows.journal" peak_rss_gb)
echo "A3_RUN1 positions=$ROWS positions_per_s=$PPS peak_rss_gb=$PEAK sha256=$S1"
echo "A2_RUN2 sha256=$S2"
[ "$S1" = "$S2" ] || fail "A2(a): two runs of the same window set differ"
echo "A2_TWO_RUNS identical=1 sha256=$S1"

echo "=== A2b: small vs large --rss-gb cap on a fixed 50k set"
run cap2 "$WORK/w50k.jsonl" 2 "$WORK/cap2.rows"
run cap16 "$WORK/w50k.jsonl" 16 "$WORK/cap16.rows"
run cap8 "$WORK/w50k.jsonl" 8 "$WORK/cap8.rows"
C2=$(sha "$WORK/cap2.rows"); C16=$(sha "$WORK/cap16.rows"); C8=$(sha "$WORK/cap8.rows")
echo "A2_CAP rss2=$C2 rss8=$C8 rss16=$C16"
[ "$C2" = "$C16" ] && [ "$C2" = "$C8" ] || fail "A2(c): the RSS cap changed the bytes"
echo "A2_RSS_CAP_PARITY identical=1 sha256=$C2 caps_gb=2,8,16"

echo "=== A2d: the binary window batch and the JSONL window stream agree"
python3 "$HERE/consult_windows.py" "$WORK/w50k.bin" --positions 50000 --len 2048 \
  --offset 3000000512 | tee -a "$WORK/windows.log"
run fmt "$WORK/w50k.bin" 8 "$WORK/fmt.rows" --bin
SF=$(sha "$WORK/fmt.rows")
echo "A2_WINDOW_FORMAT jsonl=$C8 bin=$SF"
[ "$C8" = "$SF" ] || fail "A2: the binary and JSONL window forms differ"
echo "A2_WINDOW_FORMAT_IDENTICAL jsonl_and_bin=1 sha256=$SF"

echo "=== A2c: --rss-gb 16 on the FULL 1M set (same bytes as --rss-gb 8?)"
run big "$WORK/w1m.jsonl" 16 "$WORK/big.rows"
SB=$(sha "$WORK/big.rows")
echo "A2_FULL_1M rss8=$S1 rss16=$SB"
[ "$S1" = "$SB" ] || fail "A2(c): the RSS cap changed the 1M-position output"
echo "A2_RSS_CAP_PARITY_1M identical=1 sha256=$S1"

echo "=== A4: kill mid-run, then --resume"
OUT="$WORK/a4.rows"
rm -f "$OUT" "$OUT.journal"
# SIGTERM at 95 s: the run stops wherever it is, with whatever it had already
# appended - and only the committed prefix is durable, which is the point.
# --chunk-rows 200000 (a commit every ~40 s at this rate) makes sure the kill
# leaves a REAL torn tail rather than landing on a commit boundary, so the
# truncation path is exercised rather than assumed.
KILL_CHUNK=200000
setsid timeout --kill-after=10 95 \
  "$XSA" consult --container "$CONTAINER" --corpus "$CORPUS" \
  --windows "$WORK/w1m.jsonl" --output "$OUT" --rss-gb 8 -j "$JOBS" \
  --chunk-rows "$KILL_CHUNK"
rc=$?
no_survivors
echo "A4_KILL exit=$rc (124 = the harness's SIGTERM, the crash this gate needs)"
APPENDED=$(stat -c %s "$OUT")
COMMITS=$(grep -c '^CONSULT_COMMIT ' "$OUT.journal")
LASTLEN=$(field "$OUT.journal" output_bytes)
echo "A4_KILL file_bytes=$APPENDED last_commit_output_bytes=$LASTLEN commits=$COMMITS"
[ "$COMMITS" -ge 1 ] || fail "A4: the killed run committed nothing (kill it later)"
setsid timeout --kill-after=10 7200 \
  "$XSA" consult --container "$CONTAINER" --corpus "$CORPUS" \
  --windows "$WORK/w1m.jsonl" --output "$OUT" --rss-gb 8 -j "$JOBS" --resume
rc=$?
no_survivors
[ $rc -eq 0 ] || fail "A4: --resume exited $rc"
S4=$(sha "$OUT")
R4=$(field "$OUT.journal" positions)
TRUNC=$(grep -oE 'truncated_bytes=[0-9]+' "$OUT.journal" | head -1)
echo "A4_RESUME positions=$R4 sha256=$S4 $TRUNC"
[ "$S4" = "$S1" ] || fail "A4: resumed output differs from the uninterrupted run"
[ "$R4" = "$ROWS" ] || fail "A4: resumed row count $R4 != $ROWS"
echo "A4_RESUME_IDENTICAL to_uninterrupted_run=1 positions=$R4 sha256=$S4"

echo "=== A2/A3/A4 SUMMARY"
echo "A2_DETERMINISM two_runs=1 kill_resume=1 rss_cap_2v8v16=1 window_format=1 sha256=$S1"
echo "A3_BOUNDED positions=$ROWS positions_per_s=$PPS peak_rss_gb=$PEAK cap_gb=8 jobs=$JOBS"
echo "A4_CRASH_RESUME positions=$R4 sha256=$S4"
