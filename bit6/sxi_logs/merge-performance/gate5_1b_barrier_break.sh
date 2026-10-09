#!/usr/bin/env bash
# MERGE PERFORMANCE lane - GATE 5: 1 GB BARRIER BREAK (persist-at-chunk-time)
# through the front door. The banked 1 GB rung: snap n=1,000,000,665,
# sha256 c382daa5..., chi = 283296933 (N=1000000665, R=368036609).
# Two full-chain runs on the same snap, 16 chunks, FLAT k=16:
#   A (BEFORE): CROSS_NO_PERSIST=1 - walk-derivation loads (memory-bound pool)
#   B (AFTER):  defaults - chunker-persisted order columns, pure-read loads
# Byte-identity A == B on every artifact + chi exact vs the banked constant.
# The deliverable: the before/after load-phase walls (LOAD_SIDE/LOAD_STATS).
set -uo pipefail
W=/home/erikg/worktrees/sxgc/pi-worktree-721d9f35-fd8d-4651-85a3-3995854b08f7-s0-0
XSA=$W/xsa/target/release/xsa
SNAP=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt
J=$W/bit6/sxi_logs/merge-performance
CAP_GB=${CAP_GB:-8}
CHUNKS=${CHUNKS:-16}
KWAY=${KWAY:-16}
TIMEOUT_S=${TIMEOUT_S:-14400}
mkdir -p "$J"
echo "=== 1b gate start $(date -Is) pid=$$ caps: ulimit -v ${CAP_GB} GiB chunks=$CHUNKS kway=$KWAY"
for run in A B; do
  SCR=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-$run
  rm -rf "$SCR"; mkdir -p "$SCR"
  if [ "$run" = A ]; then export CROSS_NO_PERSIST=1; else unset CROSS_NO_PERSIST; fi
  timeout "$TIMEOUT_S" "$XSA" build --input "$SNAP" --scratch "$SCR" --snap-1e \
      --memory-gb "$CAP_GB" --threads 48 --chunks "$CHUNKS" --kway "$KWAY" \
      > "$J/gate1b-$run.driver.log" 2>&1
  rc=$?
  echo "=== run $run rc=$rc $(date -Is)"
  [ "$rc" -ne 0 ] && { echo "RUN_${run}_FAIL"; tail -15 "$J/gate1b-$run.driver.log"; exit 1; }
done
echo "=== byte-identity A (derive) == B (persist) on every artifact"
pass=0; fail=0
for f in rlebwt rlebwt.meta ssa ssa_t pftext pfck; do
  cmp "/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-A/merged/frag.$f" \
      "/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-B/merged/frag.$f" \
      && { echo "ONEB_MERGE_$f BYTE_IDENTICAL"; pass=$((pass+1)); } \
      || { echo "ONEB_MERGE_$f MISMATCH"; fail=$((fail+1)); }
done
for f in ri4 head_sa agg sA; do
  cmp "/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-A/finish/frag.$f" \
      "/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-B/finish/frag.$f" \
      && { echo "ONEB_FINISH_$f BYTE_IDENTICAL"; pass=$((pass+1)); } \
      || { echo "ONEB_FINISH_$f MISMATCH"; fail=$((fail+1)); }
done
[ "$fail" -eq 0 ] || { echo "BYTE_IDENTITY_FAIL pass=$pass fail=$fail"; exit 1; }
for run in A B; do
  grep -h "chi = " /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-$run/sweep.log | tail -1 | tee -a "$J/gate1b.cmp.log"
  grep -qF 'chi = 283296933 (N=1000000665, R=368036609)' /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-$run/sweep.log \
    && echo "ONEB_${run}_CHI_283296933_OK" || { echo "CHI_MISMATCH_$run"; exit 1; }
done
echo "=== the barrier break: load-phase walls (before = derivation pool, after = pure reads)"
for run in A B; do
  echo "-- run $run"
  grep -E "PHASE merge END|LOAD_STATS" /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-$run/merge.log | tee -a "$J/gate1b.cmp.log"
  grep -c "LOAD_SIDE.*persisted-reads" /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-$run/merge.log
  grep CROSS_KWAY /mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-$run/merge.log | tail -1 | grep -oE "wall_total=[0-9.]+" | tee -a "$J/gate1b.cmp.log"
done | tee -a "$J/gate1b.cmp.log"
echo "MERGPERF_GATE5_1B_DONE pass=$pass fail=$fail"
