#!/usr/bin/env bash
# MERGE PERFORMANCE lane - GATE 2 (corpus-referenced chunks): fragment scale,
# referenced text (chunk-i.ref; no per-side text copies), through the
# consolidated `xsa build --input`. Byte-identity is absolute (8/8 vs the
# banked reference + chi = 306164765). DISC ACCOUNTING: chunk artifact bytes
# and scratch peak vs the gate-1 sidecar-mode run (mergperf-frag).
set -uo pipefail
W=/home/erikg/worktrees/sxgc/pi-worktree-721d9f35-fd8d-4651-85a3-3995854b08f7-s0-0
XSA=$W/xsa/target/release/xsa
TXT=/home/erikg/sxgc-piletest/pile-frag.txt
BANK=/home/erikg/cross-lcp-run/merged
SCR=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-frag-ref
J=$W/bit6/sxi_logs/merge-performance
CAP_GB=${CAP_GB:-6}
CHUNKS=${CHUNKS:-32}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-43200}
mkdir -p "$J"
rm -rf "$SCR"; mkdir -p "$SCR"
rm -f "$J/frag-gate2.driver.log" "$J/frag-gate2.cmp.log"

echo "=== gate start $(date -Is) pid=$$ caps: ulimit -v ${CAP_GB} GiB, timeout ${TIMEOUT_S}s (corpus-referenced mode)"
echo "=== xsa provenance: $(sha256sum "$XSA" | cut -c1-16)... (bundle: sharded emission + referenced chunks)"
df -h /mnt/nvme2n1 | tail -1
timeout "$TIMEOUT_S" "$XSA" build --input "$TXT" --scratch "$SCR" --snap-1e \
    --memory-gb "$CAP_GB" --threads "$THREADS" --chunks "$CHUNKS" \
    > "$J/frag-gate2.driver.log" 2>&1
rc=$?
echo "=== xsa build rc=$rc $(date -Is)"
[ "$rc" -ne 0 ] && { echo BUILD_FAIL; tail -20 "$J/frag-gate2.driver.log"; exit 1; }

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
grep -h "chi = " "$SCR/sweep.log" | tail -1 | tee -a "$J/frag-gate2.cmp.log"
grep -qF 'chi = 306164765 (N=1082130213, R=397723010)' "$SCR/sweep.log" \
    && echo "CONSOLIDATED_CHI_306164765_OK" || { echo CHI_MISMATCH; exit 1; }

echo "=== DISC ACCOUNTING (corpus-referenced vs sidecar mode)"
echo "REF chunks dir bytes:   $(du -sb "$SCR/chunks" | cut -f1)  ($(ls "$SCR/chunks" | grep -c '\.ref$') ref sidecars)"
echo "REF chunk .crle bytes:  $(du -cb "$SCR"/chunks/chunk-*.crle | tail -1 | cut -f1)"
du -sb "$SCR/chunks" | tee -a "$J/frag-gate2.cmp.log"
echo "=== per-phase telemetry"
grep -E "^(XSA_BUILD START|SNAP_OK|REMAP|PHASE .* (START|END)|MERGE cross_pairs|CROSS_PF_EMIT|PEAK_DISC|XSA_BUILD_DONE)" \
    "$SCR/xsa-build.log" | tee -a "$J/frag-gate2.cmp.log"
echo "MERGPERF_GATE2_DONE pass=$pass fail=$fail"
