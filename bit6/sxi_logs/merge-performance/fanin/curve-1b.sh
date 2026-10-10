#!/usr/bin/env bash
# MERGE-PERF FAN-IN lane: 1GB WALK CURVE SWEEP (same binary, dense).
# Steering: the walk-cost curve must be SAME-BINARY (the old 57%/1597 s k=16
# figure is an SXP4/LRU-lineage confound). All rows here are a7ec3df9 at 1 GB.
# Controlled points separating (side size) vs (k) vs (total live bytes):
#   k=16 @ 62.5 MiB sides (--chunks 16)   <- the anchor the steering demands
#   k=20 @ 50 MiB sides   (--chunk-mb 50)
#   k=5  @ 200 MiB sides  (--chunk-mb 200)
#   k=2  @ 500 MiB sides  (--chunks 2)
# (k=10 @ 100 MiB is already measured: gate-1b-flat10-d.)
# Every run: byte-identity vs the banked mergperf-1b-B + chi=283296933 exact,
# plus cross-merge wall, M2 POOL_STATS hit rate, peak merge disc.
# Measurement gates: SEQUENTIAL and ALONE. Caps per run.
set -uo pipefail
XSA=/home/erikg/sxgc/xsa/target/release/xsa
SNAP=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt
REF=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-B
BASE=/mnt/nvme2n1/erikg/extcols-scratch
J=/home/erikg/sxgc/bit6/sxi_logs/merge-performance/fanin
OUT=$J/curve-1b.results
CAP_GB=${CAP_GB:-8}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-7200}
mkdir -p "$J"
: > "$OUT"
note() { echo "$@" | tee -a "$OUT"; }

run_one() {
  local name="$1"; shift
  local SCR="$BASE/$name"
  rm -rf "$SCR"; mkdir -p "$SCR"
  note "### RUN $name $(date -Is)"
  ( ulimit -v $((CAP_GB*1024*1024)) ; exec timeout --kill-after=60 "$TIMEOUT_S" "$XSA" build \
      --input "$SNAP" --scratch "$SCR" --snap-1e --memory-gb "$CAP_GB" --threads "$THREADS" \
      --scratch-free-pct 0 "$@" ) > "$SCR/driver.log" 2>&1
  local rc=$?
  note "rc=$rc $(date -Is)"
  if [ "$rc" -ne 0 ]; then note "RUN_${name}_FAIL rc=$rc"; tail -25 "$SCR/driver.log" | tee -a "$OUT"; return 1; fi
  local pass=0 fail=0
  for f in rlebwt rlebwt.meta ssa ssa_t pftext pfck; do
    cmp "$SCR/merged/frag.$f" "$REF/merged/frag.$f" >/dev/null 2>&1 && pass=$((pass+1)) || { note "MISMATCH merged/frag.$f"; fail=$((fail+1)); }
  done
  for f in ri4 head_sa agg sA; do
    cmp "$SCR/finish/frag.$f" "$REF/finish/frag.$f" >/dev/null 2>&1 && pass=$((pass+1)) || { note "MISMATCH finish/frag.$f"; fail=$((fail+1)); }
  done
  note "BYTE_IDENTITY pass=$pass fail=$fail"
  grep -qF 'chi = 283296933 (N=1000000665, R=368036609)' "$SCR/sweep.log" 2>/dev/null \
    && note "CHI_EXACT_OK" || { note "CHI_MISMATCH"; fail=$((fail+1)); }
  grep -h "MERGE_PEAK_DISC_USED_BYTES\|PEAK_DISC_USED_BYTES" "$SCR/xsa-build.log" 2>/dev/null | tee -a "$OUT"
  grep -h "PHASE .* END" "$SCR/xsa-build.log" 2>/dev/null | sed 's/.*PHASE //' | tee -a "$OUT"
  grep -h "CROSS_PHASE cross-merge" "$SCR/merge.log" 2>/dev/null | sed 's/ *peak.*//' | tee -a "$OUT"
  grep -h "CROSS_KWAY" "$SCR/merge.log" 2>/dev/null | sed -E 's#(left|right|out)=/[^ ]*/([^/ ]+)#\1=\2#g' | tee -a "$OUT"
  # M2 pool hit rate: the fd=4 POOL_STATS lines (last two cover the walk).
  grep -h "POOL_STATS fd=4" "$SCR/merge.log" 2>/dev/null | tee -a "$OUT"
  [ "$fail" -eq 0 ] && note "CURVE_${name}_GREEN" || note "CURVE_${name}_RED"
}

run_one curve-1b-k16 --chunks 16 --kway 16
run_one curve-1b-k20 --chunk-mb 50 --kway 20
run_one curve-1b-k5  --chunk-mb 200 --kway 5
run_one curve-1b-k2  --chunks 2 --kway 2

note "CURVE_1B_DONE $(date -Is)"
