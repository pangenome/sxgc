#!/usr/bin/env bash
# MERGE PERFORMANCE lane - GATE 3 (FLAT k-way, the headline row): fragment
# scale, ONE 32-way merge straight from the referenced chunks (--kway 32),
# through the consolidated `xsa build --input`. Byte-identity is absolute:
# the flat artifact must byte-reproduce the certified tree artifact
# (8/8 vs the banked reference + chi = 306164765). The wall here prices the
# pile at its I/O floor.
set -uo pipefail
W=/home/erikg/worktrees/sxgc/pi-worktree-721d9f35-fd8d-4651-85a3-3995854b08f7-s0-0
XSA=$W/xsa/target/release/xsa
TXT=/home/erikg/sxgc-piletest/pile-frag.txt
BANK=/home/erikg/cross-lcp-run/merged
SCR=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-frag-flat
J=$W/bit6/sxi_logs/merge-performance
CAP_GB=${CAP_GB:-6}
CHUNKS=${CHUNKS:-32}
KWAY=${KWAY:-32}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-43200}
mkdir -p "$J"
rm -rf "$SCR"; mkdir -p "$SCR"
rm -f "$J/frag-gate3.driver.log" "$J/frag-gate3.cmp.log"

echo "=== gate start $(date -Is) pid=$$ caps: ulimit -v ${CAP_GB} GiB, timeout ${TIMEOUT_S}s (FLAT kway=${KWAY})"
echo "=== xsa provenance: $(sha256sum "$XSA" | cut -c1-16)... (bundle: sharded emission + referenced chunks + flat k-way)"
df -h /mnt/nvme2n1 | tail -1
timeout "$TIMEOUT_S" "$XSA" build --input "$TXT" --scratch "$SCR" --snap-1e \
    --memory-gb "$CAP_GB" --threads "$THREADS" --chunks "$CHUNKS" --kway "$KWAY" \
    > "$J/frag-gate3.driver.log" 2>&1
rc=$?
echo "=== xsa build rc=$rc $(date -Is)"
[ "$rc" -ne 0 ] && { echo BUILD_FAIL; tail -20 "$J/frag-gate3.driver.log"; exit 1; }

echo "=== byte-identity vs the banked fragment reference (flat must equal the certified tree artifact)"
pass=0; fail=0
for f in rlebwt rlebwt.meta ssa ssa_t; do
    if cmp "$SCR/merged/frag.$f" "$BANK/frag.$f"; then
        echo "FLAT_MERGE_$f BYTE_IDENTICAL_TO_BANKED"; pass=$((pass+1))
    else echo "FLAT_MERGE_$f MISMATCH"; fail=$((fail+1)); fi
done
for f in ri4 head_sa agg sA; do
    if cmp "$SCR/finish/frag.$f" "$BANK/fresh.$f"; then
        echo "FLAT_FINISH_$f BYTE_IDENTICAL_TO_BANKED"; pass=$((pass+1))
    else echo "FLAT_FINISH_$f MISMATCH"; fail=$((fail+1)); fi
done
[ "$fail" -eq 0 ] || { echo "BYTE_IDENTITY_FAIL pass=$pass fail=$fail"; exit 1; }
grep -h "chi = " "$SCR/sweep.log" | tail -1 | tee -a "$J/frag-gate3.cmp.log"
grep -qF 'chi = 306164765 (N=1082130213, R=397723010)' "$SCR/sweep.log" \
    && echo "FLAT_CHI_306164765_OK" || { echo CHI_MISMATCH; exit 1; }

echo "=== flat telemetry (CROSS_KWAY line + per-phase + pools)"
grep -E "CROSS_KWAY" "$SCR/merge.log" | tail -1 | tee -a "$J/frag-gate3.cmp.log"
grep -E "CROSS_PHASE|POOL_STATS|XMERG_TIMING" "$SCR/merge.log" | tee -a "$J/frag-gate3.cmp.log"
grep -E "^(XSA_BUILD START|SNAP_OK|REMAP|PHASE .* (START|END)|MERGE cross|CROSS_PF_EMIT|PEAK_DISC|XSA_BUILD_DONE)" \
    "$SCR/xsa-build.log" | tee -a "$J/frag-gate3.cmp.log"
df -h /mnt/nvme2n1 | tail -1
echo "MERGPERF_GATE3_DONE pass=$pass fail=$fail"
