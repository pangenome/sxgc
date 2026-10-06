#!/usr/bin/env bash
# WIDENING VALIDATION at fragment scale: the widened chunk route (SXCR v2
# 64-bit lens, SXS2 64-bit positions, u64 order vectors) must reproduce the
# BANKED merged four files byte-identically from the same real corpus bytes
# (pile-frag.txt, the 1.08 GB pile prefix the banked artifacts came from,
# production frag.remap). This is the v2 semantic differential against the
# v1-produced banked artifacts.
set -uo pipefail
BIN=/tmp/extcols/bin
CF=$BIN/chunk_frontend
TXT=/home/erikg/sxgc-piletest/pile-frag.txt
REMAP=/home/erikg/sxgc/vendor/chunk-merge-v3/merged/frag.remap
BANK=/home/erikg/cross-lcp-run/merged
W=/tmp/extcols/v2frag
mkdir -p "$W/chunks" "$W/merged" "$W/mwork"
N=$(stat -c %s "$TXT")

echo "=== v2 chunk_frontend"
/usr/bin/time -v -o "$W/chunk.time" "$CF" "$TXT" 16 "$W/chunks" "$REMAP" > "$W/chunk.log" 2>&1 || { echo CHUNK_FAIL; exit 1; }
grep CHUNKS_PASS "$W/chunk.log"
echo "=== v2 merge --tree --emit-pf"
/usr/bin/time -v -o "$W/merge.time" "$BIN/cross_lcp_merge" --tree "$W/chunks" 16 "$N" "$W/merged/frag" \
    --threads 48 --work "$W/mwork" --emit-pf > "$W/merge.log" 2>&1 || { echo MERGE_FAIL; tail -3 "$W/merge.log"; exit 1; }
grep CROSS_PF_EMIT "$W/merge.log" | tail -1
for f in rlebwt rlebwt.meta ssa ssa_t; do
    cmp "$W/merged/frag.$f" "$BANK/frag.$f" && echo "V2_MERGE_$f BYTE_IDENTICAL_TO_BANKED" || { echo "V2_$f MISMATCH"; exit 1; }
done
# also gate the v2 sidecars: the emitted text+checkpoints must feed the
# finish identically (the banked fresh.agg came from the banked ri4/head_sa)
BIN3=/tmp/extcols/bin3
/usr/bin/time -v -o "$W/slim.time" "$BIN3/slim_dump" --slim --resolve-ri4 --ri4 "$BANK/fresh.ri4" \
    --head-sa "$BANK/fresh.head_sa" -t 48 \
    --pf-text "$W/merged/frag.pftext" --pf-checkpoints "$W/merged/frag.pfck" \
    -o "$W/adopt.agg" > "$W/slim.log" 2>&1 || { echo SLIM_FAIL; exit 1; }
cmp "$W/adopt.agg" "$BANK/fresh.agg" && echo "V2_SIDECARS_AGG_BYTE_IDENTICAL_TO_BANKED"
grep -E "Elapsed|Maximum resident" "$W/merge.time" | grep -v aver
echo V2FRAG_VALIDATION_DONE
