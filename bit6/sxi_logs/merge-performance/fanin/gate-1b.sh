#!/usr/bin/env bash
# MERGE-PERF FAN-IN lane: 1GB STRONG GATE (item 2).
# Fixed 1GB fixture (n=1,000,000,665, banked chi=283296933, R=368036609),
# chunked at 100MB -> 10 chunks. Two shapes, same binary, same window:
#   FLAT k=10 : ONE 10-way merge straight from the chunks
#   FAN-IN k=4: fixed 100MB chunks, repeated 4-way fan-in (levels 10 -> 3 -> 1)
# Both must byte-reproduce the banked certified reference mergperf-1b-B
# (all 6 merged + all 4 finish artifacts) and carry chi = 283296933 exact.
# The fan-in's level-0 sides are k x chunk = ~400MB (the top-level-side
# datapoint for the walk-vs-side-size curve).
# Measurement gate: runs SEQUENTIALLY and ALONE. Caps on every run.
set -uo pipefail
XSA=/home/erikg/sxgc/xsa/target/release/xsa
SNAP=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt
REF=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-B
BASE=/mnt/nvme2n1/erikg/extcols-scratch
J=/home/erikg/sxgc/bit6/sxi_logs/merge-performance/fanin
OUT=$J/gate-1b.results
CAP_GB=${CAP_GB:-8}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-7200}
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
  ( ulimit -v $((CAP_GB*1024*1024)) ; exec timeout --kill-after=60 "$TIMEOUT_S" "${envs[@]}" "$XSA" build \
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
  grep -h "chi = " "$SCR/sweep.log" 2>/dev/null | tail -1 | tee -a "$OUT"
  grep -qF 'chi = 283296933 (N=1000000665, R=368036609)' "$SCR/sweep.log" 2>/dev/null \
    && note "CHI_EXACT_OK" || { note "CHI_MISMATCH"; fail=$((fail+1)); }
  grep -h "MERGE_PEAK_DISC_USED_BYTES\|_PEAK_DISC\|PEAK_DISC" "$SCR/xsa-build.log" 2>/dev/null | tee -a "$OUT"
  grep -h "PHASE .* END" "$SCR/xsa-build.log" 2>/dev/null | sed 's/.*PHASE //' | tee -a "$OUT"
  grep -h "CROSS_PHASE" "$SCR/merge.log" 2>/dev/null | sed 's/ *peak.*//' | tee -a "$OUT"
  grep -h "CROSS_KWAY\|CROSS_PAIR" "$SCR/merge.log" 2>/dev/null | sed -E 's#(left|right|out)=/[^ ]*/([^/ ]+)#\1=\2#g' | tee -a "$OUT"
  grep -h "LOAD_STATS\|POOL_STATS" "$SCR/merge.log" 2>/dev/null | tee -a "$OUT"
  [ "$fail" -eq 0 ] && note "GATE_${name}_GREEN" || note "GATE_${name}_RED"
}

run_one gate-1b-flat10-d dense --chunks 10 --kway 10
run_one gate-1b-k4-d     dense --chunk-mb 100 --kway 4 --fanin

note "GATE_1B_DONE $(date -Is)"
