#!/usr/bin/env bash
# MERGE PERFORMANCE lane - GATE 1 (parallel emission): fragment-scale
# byte-identity + the fat-pair wall measurement, through the consolidated
# `xsa build --input` (sharded order-boundary emission, S=--threads).
# Banked baseline (b031c61, serial walks): merge tree 6:57:38, peak RSS
# 2.53 GB, chi = 306164765, 8/8 byte-identity. This gate must reproduce all
# eight artifacts byte-for-byte and journal the per-phase walls for the
# deliverable table (cross-merge / emit phases were the 2-5-core serial hot
# phases; none may exceed ~10% of merge wall under 8 cores after this change).
set -uo pipefail
W=/home/erikg/worktrees/sxgc/pi-worktree-721d9f35-fd8d-4651-85a3-3995854b08f7-s0-0
XSA=$W/xsa/target/release/xsa
TXT=/home/erikg/sxgc-piletest/pile-frag.txt
BANK=/home/erikg/cross-lcp-run/merged
SCR=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-frag
J=$W/bit6/sxi_logs/merge-performance
CAP_GB=${CAP_GB:-6}
CHUNKS=${CHUNKS:-32}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-43200}
mkdir -p "$J"
rm -rf "$SCR"; mkdir -p "$SCR"
rm -f "$J/frag-gate1.driver.log" "$J/frag-gate1.cmp.log"

echo "=== gate start $(date -Is) pid=$$ caps: ulimit -v ${CAP_GB} GiB (per phase), timeout ${TIMEOUT_S}s"
echo "=== xsa provenance: $(sha256sum "$XSA" | cut -c1-16)... (bundle: sharded emission)"
df -h /mnt/nvme2n1 | tail -1
timeout "$TIMEOUT_S" "$XSA" build --input "$TXT" --scratch "$SCR" --snap-1e \
    --memory-gb "$CAP_GB" --threads "$THREADS" --chunks "$CHUNKS" \
    > "$J/frag-gate1.driver.log" 2>&1
rc=$?
echo "=== xsa build rc=$rc $(date -Is)"
[ "$rc" -ne 0 ] && { echo BUILD_FAIL; tail -20 "$J/frag-gate1.driver.log"; exit 1; }

echo "=== byte-identity vs the banked fragment reference"
pass=0; fail=0
for f in rlebwt rlebwt.meta ssa ssa_t; do
    if cmp "$SCR/merged/frag.$f" "$BANK/frag.$f"; then
        echo "CONSOLIDATED_MERGE_$f BYTE_IDENTICAL_TO_BANKED"; pass=$((pass+1))
    else echo "CONSOLIDATED_MERGE_$f MISMATCH"; fail=$((fail+1)); fi
done
for f in ri4 head_sa agg sA; do
    if cmp "$SCR/finish/frag.$f" "$BANK/fresh.$f"; then
        echo "CONSOLIDATED_FINISH_$f BYTE_IDENTICAL_TO_BANKED"; pass=$((pass+1))
    else echo "CONSOLIDATED_FINISH_$f MISMATCH"; fail=$((fail+1)); fi
done
[ "$fail" -eq 0 ] || { echo "BYTE_IDENTITY_FAIL pass=$pass fail=$fail"; exit 1; }

echo "=== pf sidecar journal"
stat -c "PFTEXT bytes=%s (want n=1082130213)" "$SCR/merged/frag.pftext"
grep -h "CROSS_PF_EMIT" "$SCR/merge.log" | tail -1
grep -h "chi = " "$SCR/sweep.log" | tail -1 | tee -a "$J/frag-gate1.cmp.log"
grep -qF 'chi = 306164765 (N=1082130213, R=397723010)' "$SCR/sweep.log" \
    && echo "CONSOLIDATED_CHI_306164765_OK" || { echo CHI_MISMATCH; exit 1; }

echo "=== per-phase telemetry (uniform xsa-build.log)"
grep -E "^(XSA_BUILD START|SNAP_OK|REMAP|PHASE .* (START|END)|MERGE cross_pairs|CROSS_PF_EMIT|PEAK_DISC|XSA_BUILD_DONE)" \
    "$SCR/xsa-build.log" | tee -a "$J/frag-gate1.cmp.log"
echo "=== merge per-phase serial-core audit (fat pairs; CROSS_PHASE walls)"
grep -E "CROSS_PHASE (cross-merge|emit-count|emit-write|sxcr-count|sxcr-write|anchor\+repair|sidecar-write|emit-pf|hash-build|m2-build)" \
    "$SCR/merge.log" | tee -a "$J/frag-gate1.cmp.log"
df -h /mnt/nvme2n1 | tail -1
echo "MERGPERF_GATE1_DONE pass=$pass fail=$fail"
