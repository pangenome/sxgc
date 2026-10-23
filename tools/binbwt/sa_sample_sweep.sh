#!/usr/bin/env bash
# sa_sample_sweep.sh - the measured curve "placement cost vs sampling constant"
# (charter Phase 4), on a real fixture, through the REAL binaries.
#
#   bash tools/binbwt/sa_sample_sweep.sh {100m|1b} BUILD_DIR ROOT K...
#
# For every k in the argument list (k=0 = the product default, OFF):
#   producer = cross_lcp_merge --binbwt ... [--sa-sample-k k --sa-sample-out F]
#   finish   = pile_finish (byte-identical container expected for EVERY k)
#   then: cmp against the banked container, sa_anchor_verify (order-only) on the
#   anchor file, and every counter line (BIN_WORK / SLIM_FP / SA_ANCHOR_DONE)
#   kept in <ROOT>/k<k>/. One table row per k.
#
# Walls are LABELLED with the load: this box carries the live 100 GB rung, so
# no wall here is a quiet-box number.
set -uo pipefail
cd "$(dirname "$0")/../.."
SIZE="${1:?usage: sa_sample_sweep.sh 100m-or-1b BUILD ROOT K...}"
BUILD="$(cd "${2:?}" && pwd)"
ROOT="${3:?}"; shift 3
BASE=/mnt/nvme2n1/erikg
case "$SIZE" in
  100m) FIX=$BASE/extcols-scratch/mergperf-100m-snap.txt
        BANK=$BASE/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2
        CHI=29716349; MEMGB=24 ;;
  1b)   FIX=$BASE/extcols-scratch/mergperf-1b-snap.txt
        BANK=$BASE/extcols-scratch/pilev2-g2-1b/frag.sxi2
        CHI=283296933; MEMGB=40 ;;
  *) echo "unknown size $SIZE" >&2; exit 2 ;;
esac
REMAP=$BASE/sol6c-gate100m/remap-256.bin
THREADS=${THREADS:-96}
N=$(stat -c%s "$FIX")
WIT=$(( (MEMGB / 2) * (1<<30) ))
mkdir -p "$ROOT"
printf '%-6s %-12s %-14s %-9s %-9s %-9s %-12s %-8s %s\n' \
  k anchor_bytes anchor_entries prod_wall_s finish_wall_s total_wall_s cmp verified_symbols verdict

for K in "$@"; do
  D="$ROOT/k$K"; rm -rf "$D"; mkdir -p "$D/merged"
  P="$D/merged/frag"; A="$D/anchors.sxsa"
  EXTRA=(); if [ "$K" != "0" ]; then EXTRA=(--sa-sample-k "$K" --sa-sample-out "$A"); fi
  if [ "$K" = "0" ]; then A=""; fi
  LOAD=$(cut -d' ' -f1 /proc/loadavg)
  {
    CROSS_POOL_SLOTS=8388608 CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=1100 \
      /usr/bin/time -f "PRODUCER_WALL=%e PRODUCER_MAXRSS_KB=%M" \
      "$BUILD/binbwt" --binbwt "$FIX" --remap "$REMAP" --out-prefix "$P" \
      --threads "$THREADS" --bin-live-cap "$N" "${EXTRA[@]}" || echo PRODUCER_RC=$?
  } > "$D/producer.log" 2>&1 &
  PP=$!
  {
    /usr/bin/time -f "FINISH_WALL=%e FINISH_MAXRSS_KB=%M" \
      "$BUILD/pile_finish" --prefix "$P" --corpus "$FIX" --remap "$REMAP" \
      --output "$D/out.sxi2" --threads "$THREADS" --poll-timeout 7200 \
      --wit-ram-budget "$WIT" --chi-expect "$CHI" || echo FINISH_RC=$?
  } > "$D/finish.log" 2>&1 &
  FP=$!
  T0=$(date +%s.%N)
  wait $PP; PRC=$?
  wait $FP; FRC=$?
  T1=$(date +%s.%N)
  cmp -s "$D/out.sxi2" "$BANK"; CMP=$?
  PW=$(sed -n 's/.*PRODUCER_WALL=\([0-9.]*\).*/\1/p' "$D/producer.log" | tail -1)
  FW=$(sed -n 's/.*FINISH_WALL=\([0-9.]*\).*/\1/p' "$D/finish.log" | tail -1)
  VS=$(sed -n 's/.*SLIM_FP pile-finish-corpus .*verified_symbols=\([0-9]*\).*/\1/p' "$D/finish.log" | tail -1)
  AB=$(stat -c%s "$A" 2>/dev/null || echo 0)
  AE=$(sed -n 's/.*SA_ANCHOR_DONE .*entries=\([0-9]*\).*/\1/p' "$D/producer.log" | tail -1)
  VOK="-"
  if [ "$K" != "0" ]; then
    "$BUILD/sa_anchor_verify" "$A" "$FIX" "$REMAP" > "$D/verify.log" 2>&1 && VOK="order-ok" || VOK="VERIFY-FAIL"
  fi
  V=$( [ $PRC -eq 0 ] && [ $FRC -eq 0 ] && [ $CMP -eq 0 ] && [ "$VOK" != "VERIFY-FAIL" ] && echo PASS || echo FAIL )
  printf '%-6s %-12s %-14s %-9s %-9s %-9s %-12s %-8s %-8s %s (loadavg@start=%s)\n' \
    "$K" "$AB" "${AE:--}" "${PW:--}" "${FW:--}" "$(awk -v a="$T1" -v b="$T0" 'BEGIN{printf "%.1f", a-b}')" \
    "$( [ $CMP -eq 0 ] && echo identical || echo DIFF)" "${VS:--}" "$V" "$VOK" "$LOAD" \
    | tee -a "$ROOT/table.txt"
done
