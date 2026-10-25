#!/usr/bin/env bash
# entropy_bank_gate.sh — the entropy rule's BYTE-IDENTITY gate at bank scale.
#
#   bash tools/binbwt/entropy_bank_gate.sh NAME SIZE ROOT [threads] [extra producer flags...]
#
# Runs the REAL product pipeline (binbwt --binbwt -> xsa_finish) over the banked
# fixture with whatever producer flags are passed, and requires:
#
#   1. producer rc=0 and finish rc=0,
#   2. the finish's chi-rspace == the bank's chi (--chi-expect makes the finish
#      itself refuse a mismatch, and the printed line is checked here too),
#   3. sha256(product) == the banked sha256,
#   4. cmp(product, bank) == 0, byte for byte.
#
# This is the gate ENTROPY_RULE_DESIGN.md §8.4 calls the strongest gate the tree
# has: the stopping rule changes WHEN a bucket stops splitting, never WHAT the
# tree produces, so the ON arm must reproduce the bank exactly.
#
# Every wall is printed with the box's load average at the arm's start; this box
# carries foreign work and none of these is a quiet-box number.
set -uo pipefail
cd "$(dirname "$0")/../.."
NAME="${1:?usage: entropy_bank_gate.sh NAME SIZE ROOT [threads] [producer flags...]}"
SIZE="${2:?}"; ROOT="${3:?}"; TH="${4:-96}"
shift $(( $# < 4 ? $# : 4 ))
EXTRA=("$@")
BASE=/mnt/nvme2n1/erikg
REMAP=$BASE/sol6c-gate100m/remap-256.bin
BIN="${BINBWT_BIN:-$BASE/er2-build/binbwt}"
PF="${XSA_FINISH_BIN:-$BASE/sol7e-freshstart/xsa_finish}"
case "$SIZE" in
  100m) FIX=$BASE/extcols-scratch/mergperf-100m-snap.txt
        BANK=$BASE/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2
        CHI=29716349; ULIMIT=8388608
        SHA=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5 ;;
  1b)   FIX=$BASE/extcols-scratch/mergperf-1b-snap.txt
        BANK=$BASE/extcols-scratch/pilev2-g2-1b/frag.sxi2
        CHI=283296933; ULIMIT=41943040
        SHA=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f ;;
  *) echo "unknown size $SIZE" >&2; exit 2 ;;
esac
mkdir -p "$ROOT"
D="$ROOT/$NAME"; rm -rf "$D"; mkdir -p "$D"
LOAD0=$(cut -d' ' -f1 /proc/loadavg)
echo "=== GATE name=$NAME size=$SIZE threads=$TH loadavg=$LOAD0 $(date +%T) extra=${EXTRA[*]:-none}"
T0=$(date +%s.%N)
(
  ulimit -v "$ULIMIT"
  /usr/bin/time -f "PRODUCER_WALL=%e maxrss_kb=%M" -o "$D/producer.time" \
    env CROSS_POOL_SLOTS=33554432 CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=1100 \
    "$BIN" --binbwt "$FIX" --remap "$REMAP" --out-prefix "$D/frag" \
      --threads "$TH" --bin-live-cap 4294967296 "${EXTRA[@]}" \
      > "$D/producer.log" 2>&1
  echo $? > "$D/producer.rc"
) &
PID_PROD=$!
(
  /usr/bin/time -f "FINISH_WALL=%e maxrss_kb=%M" -o "$D/finish.time" \
    env CROSS_POOL_SLOTS=33554432 CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=1100 \
    "$PF" --prefix "$D/frag" --corpus "$FIX" --remap "$REMAP" \
      --output "$D/product.sxi2" --threads 96 --poll-timeout 7200 \
      --wit-ram-budget $(( 4 * 1024 * 1024 * 1024 )) --chi-expect "$CHI" \
      > "$D/finish.log" 2>&1
  echo $? > "$D/finish.rc"
) &
PID_FIN=$!
wait $PID_PROD; PRC=$(cat "$D/producer.rc")
if [ "$PRC" != 0 ]; then kill "$PID_FIN" 2>/dev/null; fi
wait $PID_FIN; FRC=$(cat "$D/finish.rc")
WALL=$(echo "$(date +%s.%N) - $T0" | bc)
PWALL=$(grep -oE 'PRODUCER_WALL=[0-9.]+' "$D/producer.time" | cut -d= -f2)
FWALL=$(grep -oE 'FINISH_WALL=[0-9.]+' "$D/finish.time" | cut -d= -f2)
CHIOUT=$(grep -oE 'chi = [0-9]+' "$D/finish.log" | tail -1 | grep -oE '[0-9]+')
SHAOUT=$(sha256sum "$D/product.sxi2" 2>/dev/null | cut -d' ' -f1)
CMP=1; cmp -s "$D/product.sxi2" "$BANK" && CMP=0
LOAD1=$(cut -d' ' -f1 /proc/loadavg)
LINE="GATE=$NAME size=$SIZE producer_rc=$PRC finish_rc=$FRC cmp=$CMP chi=$CHIOUT bank_chi=$CHI sha=$SHAOUT bank_sha=$SHA producer_wall_s=$PWALL finish_wall_s=$FWALL total_wall_s=$WALL loadavg_start=$LOAD0 loadavg_end=$LOAD1 extra=${EXTRA[*]:-none}"
printf '%s\n' "$LINE" | tee "$D/gate.tsv"
grep -E '^BIN_WORK ' "$D/producer.log" | tail -1 | tr ' ' '\n' \
  | grep -E '^(level_passes|level_elems|comparisons|symbols|leaf_sorts|leaf_elems|emitted_rows|depth_max|leaf_rows_max|entropy_evals|entropy_taken|entropy_exits|entropy_exit_rows|entropy_noprog_exits|entropy_leaf_evals|entropy_leaf_taken|entropy_leaf_exits|entropy_armed|entropy_min_bits|entropy_leaf_min_bits)=' \
  | sed 's/^/  /' || true
OK=1
[ "$PRC" = 0 ] && [ "$FRC" = 0 ] || OK=0
[ "$CHIOUT" = "$CHI" ] || OK=0
[ "$SHAOUT" = "$SHA" ] || OK=0
[ "$CMP" = 0 ] || OK=0
if [ "$OK" = 1 ]; then echo "=== GATE_OK $NAME rc=0 chi=$CHIOUT sha==bank cmp==bank"
else echo "=== GATE_FAIL $NAME (see $D)" >&2; exit 1; fi
