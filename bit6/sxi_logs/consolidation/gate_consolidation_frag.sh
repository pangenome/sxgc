#!/usr/bin/env bash
# FRAGMENT-scale BYTE-IDENTITY gate for the CONSOLIDATED chain: a single
# cargo-built `xsa build --input` command (from pushed main) must reproduce
# the banked fragment reference byte-for-byte:
#   merged:  frag.{rlebwt,rlebwt.meta,ssa,ssa_t} vs /home/erikg/cross-lcp-run/merged
#   finish:  frag.{ri4,head_sa,agg,sA} vs the bank's fresh.{ri4,head_sa,agg,sA}
#   chi = 306164765 (N=1082130213, R=397723010) exactly
# The emitted pf sidecars (frag.pftext = n bytes; frag.pfck tau-count u64s)
# are journaled; the bank predates --emit-pf, so the adopt-mode byte gate on
# agg against the banked walk-mode fresh.agg is their certification
# (bit6/sxi_logs/external-columns: lever gate + frag gate 5).
# Caps: hard ulimit -v via --memory-gb (default 6 GiB, the frag gate 5 cap)
# and an outer timeout. Scratch on nvme2n1 only. The xsa build's own disc
# preflight (>= 15% free) must PASS for this gate to run.
set -uo pipefail
W=/home/erikg/worktrees/sxgc/pi-worktree-d71c804e-9751-4699-a734-7e445af3b5de-s0-0
XSA=$W/xsa/target/release/xsa
TXT=/home/erikg/sxgc-piletest/pile-frag.txt
BANK=/home/erikg/cross-lcp-run/merged
SCR=/mnt/nvme2n1/erikg/extcols-scratch/consolidation-frag
J=$W/bit6/sxi_logs/consolidation
CAP_GB=${CAP_GB:-6}
CHUNKS=${CHUNKS:-32}
THREADS=${THREADS:-48}
TIMEOUT_S=${TIMEOUT_S:-43200}
mkdir -p "$J"
rm -rf "$SCR"; mkdir -p "$SCR"
rm -f "$J/frag-xsa-build.driver.log" "$J/frag-xsa-build.cmp.log"

echo "=== gate start $(date -Is) pid=$$ caps: ulimit -v ${CAP_GB} GiB (per phase), timeout ${TIMEOUT_S}s"
echo "=== xsa provenance: $(sha256sum "$XSA" | cut -c1-16)..."
df -h /mnt/nvme2n1 | tail -1
timeout "$TIMEOUT_S" "$XSA" build --input "$TXT" --scratch "$SCR" --snap-1e \
    --memory-gb "$CAP_GB" --threads "$THREADS" --chunks "$CHUNKS" \
    > "$J/frag-xsa-build.driver.log" 2>&1
rc=$?
echo "=== xsa build rc=$rc $(date -Is)"
[ "$rc" -ne 0 ] && { echo BUILD_FAIL; tail -20 "$J/frag-xsa-build.driver.log"; exit 1; }

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

echo "=== pf sidecar journal (bank predates --emit-pf; certified via agg)"
stat -c "PFTEXT bytes=%s (want n=1082130213)" "$SCR/merged/frag.pftext"
stat -c "PFCK bytes=%s" "$SCR/merged/frag.pfck"
grep -h "CROSS_PF_EMIT" "$SCR/merge.log" | tail -1
grep -h "chi = " "$SCR/sweep.log" | tail -1 | tee -a "$J/frag-xsa-build.cmp.log"
grep -qF 'chi = 306164765 (N=1082130213, R=397723010)' "$SCR/sweep.log" \
    && echo "CONSOLIDATED_CHI_306164765_OK" || { echo CHI_MISMATCH; exit 1; }

echo "=== per-phase telemetry (from the uniform xsa-build.log)"
grep -E "^(XSA_BUILD START|SNAP_OK|REMAP|PHASE .* (START|END)|MERGE cross_pairs|CROSS_PF_EMIT|PEAK_DISC|XSA_BUILD_DONE)" \
    "$SCR/xsa-build.log" | tee -a "$J/frag-xsa-build.cmp.log"
df -h /mnt/nvme2n1 | tail -1
echo "CONSOLIDATION_FRAG_GATE_DONE pass=$pass fail=$fail"
