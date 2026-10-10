#!/usr/bin/env bash
# MERGE-PERF FAN-IN lane: 100MB SHAPE MATRIX (item 1 of the fan-in work).
# Fixed 100MB fixture (n=100000503, banked chi=29716349) at 16 chunks.
# Shapes: flat k=16 (one level), fan-in k=4 (2 levels), fan-in k=2 (4 levels).
# Modes: dense (SXP3) and compact (SXP4). Every shape must byte-reproduce the
# banked certified flat reference (mergperf-100m-bank) on all 8 artifacts and
# carry chi = 29716349 exact.
# Measurement gates: these run SEQUENTIALLY and ALONE.
set -uo pipefail
XSA=/home/erikg/sxgc/xsa/target/release/xsa
SNAP=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-100m-snap.txt
BANK=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-100m-bank
BASE=/mnt/nvme2n1/erikg/extcols-scratch
J=/home/erikg/sxgc/bit6/sxi_logs/merge-performance/fanin
OUT=$J/matrix-100m.results
CAP_GB=${CAP_GB:-6}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-1800}
mkdir -p "$J"
: > "$OUT"

note() { echo "$@" | tee -a "$OUT"; }

run_one() {
  local name="$1" mode="$2"; shift 2
  local SCR="$BASE/$name"
  rm -rf "$SCR"; mkdir -p "$SCR"
  local envs=()
  [ "$mode" = compact ] && envs=(env CHUNK_POS_MODE=compact CROSS_POS_MODE=compact)
  note "### RUN $name mode=$mode $(date -Is)"
  # Caps: wall timeout + address-space cap (ulimit -v) + kill the whole chain.
  ( ulimit -v $((CAP_GB*1024*1024)) ; exec timeout --kill-after=30 "$TIMEOUT_S" "${envs[@]}" "$XSA" build \
      --input "$SNAP" --scratch "$SCR" --snap-1e --memory-gb "$CAP_GB" --threads "$THREADS" \
      --scratch-free-pct 0 "$@" ) > "$SCR/driver.log" 2>&1
  local rc=$?
  note "rc=$rc $(date -Is)"
  if [ "$rc" -ne 0 ]; then note "RUN_${name}_FAIL rc=$rc"; tail -20 "$SCR/driver.log" | tee -a "$OUT"; return 1; fi
  local pass=0 fail=0
  for f in rlebwt rlebwt.meta ssa ssa_t; do
    cmp "$SCR/merged/frag.$f" "$BANK/merged/frag.$f" >/dev/null 2>&1 && pass=$((pass+1)) || { note "MISMATCH merged/frag.$f"; fail=$((fail+1)); }
  done
  for f in ri4 head_sa agg sA; do
    cmp "$SCR/finish/frag.$f" "$BANK/finish/frag.$f" >/dev/null 2>&1 && pass=$((pass+1)) || { note "MISMATCH finish/frag.$f"; fail=$((fail+1)); }
  done
  note "BYTE_IDENTITY pass=$pass fail=$fail"
  local chi
  chi=$(grep -h "chi = " "$SCR/sweep.log" 2>/dev/null | tail -1)
  note "CHI $chi"
  grep -qF 'chi = 29716349 (N=100000503, R=38647515)' "$SCR/sweep.log" 2>/dev/null \
    && note "CHI_EXACT_OK" || { note "CHI_MISMATCH"; fail=$((fail+1)); }
  # Peak disc (delete-as-you-go sampled peak through the merge + end tree).
  grep -h "MERGE_PEAK_DISC_USED_BYTES\|_PEAK_DISC" "$SCR/xsa-build.log" 2>/dev/null | tee -a "$OUT"
  grep -h "PHASE .* END" "$SCR/xsa-build.log" 2>/dev/null | sed 's/.*PHASE //' | tee -a "$OUT"
  grep -h "CROSS_KWAY\|CROSS_PAIR" "$SCR/merge.log" 2>/dev/null | sed -E 's#(left|right|out)=/[^ ]*/([^/ ]+)#\1=\2#g' | tee -a "$OUT"
  grep -h "LOAD_STATS" "$SCR/merge.log" 2>/dev/null | tee -a "$OUT"
  [ "$fail" -eq 0 ] && note "SHAPE_${name}_GREEN" || note "SHAPE_${name}_RED"
}

# Dense shapes (3): flat k=16, fan-in k=4, fan-in k=2.
run_one fanin-100m-flat16-d dense --chunks 16 --kway 16
run_one fanin-100m-k4-d    dense --chunk-mb 6 --kway 4 --fanin
run_one fanin-100m-k2-d    dense --chunk-mb 6 --kway 2 --fanin
# Compact shapes (3).
run_one fanin-100m-flat16-c compact --chunks 16 --kway 16
run_one fanin-100m-k4-c     compact --chunk-mb 6 --kway 4 --fanin
run_one fanin-100m-k2-c     compact --chunk-mb 6 --kway 2 --fanin

note "MATRIX_100M_DONE $(date -Is)"
