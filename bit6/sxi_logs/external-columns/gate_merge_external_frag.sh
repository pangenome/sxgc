#!/usr/bin/env bash
# MERGE-EXTERNALIZATION GATE at fragment scale: the externalized merge (all
# per-position arrays in scratch files: M2/sparse-hash/BWT/WM/windowed
# children + repair overlay) must reproduce the BANKED merged artifacts
# byte-identically from the same real corpus bytes — merge exactness is
# verification-carried, so the RAM diet must cost ZERO output bytes.
# Run under ulimit -v 6 GiB: the RESIDENT merge peaked 35.9 GiB here and
# could not survive this cap; the externalized merge must.
set -uo pipefail
BIN=/tmp/extcols/bin
CF=$BIN/chunk_frontend
TXT=/home/erikg/sxgc-piletest/pile-frag.txt
REMAP=/home/erikg/sxgc/vendor/chunk-merge-v3/merged/frag.remap
BANK=/home/erikg/cross-lcp-run/merged
SCR=/mnt/nvme2n1/erikg/extcols-scratch/fragext
rm -rf "$SCR"; mkdir -p "$SCR/chunks" "$SCR/merged" "$SCR/mwork"
N=$(stat -c %s "$TXT")

echo "=== chunk_frontend (v2, unchanged)"
"$CF" "$TXT" 16 "$SCR/chunks" "$REMAP" > "$SCR/chunk.log" 2>&1 || { echo CHUNK_FAIL; exit 1; }
grep CHUNKS_PASS "$SCR/chunk.log"

echo "=== externalized merge --tree --emit-pf under ulimit -v 6 GiB"
( ulimit -v 6291456; export CROSS_NO_POOL=1; /usr/bin/time -v -o "$SCR/merge.time" \
  "$BIN/cross_lcp_merge" --tree "$SCR/chunks" 16 "$N" "$SCR/merged/frag" \
    --threads 48 --work "$SCR/mwork" --emit-pf > "$SCR/merge.log" 2>&1 ) \
  || { echo MERGE_FAIL; tail -5 "$SCR/merge.log"; exit 1; }
grep -c "CROSS_PAIR" "$SCR/merge.log"
grep "CROSS_PF_EMIT" "$SCR/merge.log" | tail -1
grep -o "Maximum resident set size.*" "$SCR/merge.time"

for f in rlebwt rlebwt.meta ssa ssa_t; do
    cmp "$SCR/merged/frag.$f" "$BANK/frag.$f" \
      && echo "EXTMERGE_$f BYTE_IDENTICAL_TO_BANKED" || { echo "EXTMERGE_$f MISMATCH"; exit 1; }
done
for f in pftext pfck; do
    cmp "$SCR/merged/frag.$f" "$BANK/frag.$f" 2>/dev/null \
      && echo "EXTMERGE_$f BYTE_IDENTICAL_TO_BANKED" \
      || echo "EXTMERGE_$f not present in bank (informational; gate on agg below)"
done

echo "=== adopt endpoints + slim from the externalized merge's sidecars (same cap)"
BIN3=/tmp/extcols/bin3
( ulimit -v 6291456; export CROSS_NO_POOL=1
  /usr/bin/time -v -o "$SCR/ep.time" \
  "$BIN3/rpfbwt_endpoints" "$SCR/merged/frag" "$SCR/frag-ext.ri4" "$SCR/frag-ext.head_sa" \
      --pf-text "$SCR/merged/frag.pftext" --pf-checkpoints "$SCR/merged/frag.pfck" > "$SCR/ep.log" 2>&1 ) \
  || { echo EP_FAIL; tail -3 "$SCR/ep.log"; exit 1; }
grep ENDPOINT_PASS "$SCR/ep.log" | head -1
( ulimit -v 6291456; export CROSS_NO_POOL=1; /usr/bin/time -v -o "$SCR/slim.time" \
  "$BIN3/slim_dump" --slim --resolve-ri4 --ri4 "$SCR/frag-ext.ri4" --head-sa "$SCR/frag-ext.head_sa" -t 48 \
  --pf-text "$SCR/merged/frag.pftext" --pf-checkpoints "$SCR/merged/frag.pfck" \
    --output "$SCR/frag-ext.agg" > "$SCR/slim.log" 2>&1 ) \
  || { echo SLIM_FAIL; tail -3 "$SCR/slim.log"; exit 1; }
grep -o "Maximum resident set size.*" "$SCR/slim.time"
cmp "$SCR/frag-ext.agg" "$BANK/fresh.agg" \
  && echo "EXTMERGE_AGG BYTE_IDENTICAL_TO_BANKED" || { echo "EXTMERGE_AGG MISMATCH"; exit 1; }
cp "$SCR/merge.log" "$SCR/merge.time" "$SCR/slim.log" "$SCR/slim.time" \
   /home/erikg/worktrees/sxgc/pi-worktree-c6c88936-183c-4f6d-a7cf-c7cf3da715d7-s0-0/bit6/sxi_logs/external-columns/logs/ 2>/dev/null
echo "EXTMERGE_FRAG_VALIDATION_DONE"
