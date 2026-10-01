#!/usr/bin/env bash
# Stable journal and artifact paths for the 16-chunk tree gate.
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
journal=$root/bit6/sxi_logs/merge-tree
artifact=/home/erikg/sxgc/vendor/chunk-merge-tree
reference=/home/erikg/sxgc/vendor/chunk-merge-v3/reference/parse
chunks=/home/erikg/sxgc/vendor/chunk-merge-v3/chunks
mkdir -p "$artifact"
for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do test -s "${reference}${ext}"; done
python3 bit6/sxi_logs/chunk-merge-v2/verify_chunks.py "$chunks" 16 1082130213 > "$journal/chunk-header-gate.log"
c++ -O3 -std=c++17 bit6/chunk_bcr_merge.cpp -o "$artifact/chunk_bcr_merge"
CHUNK_FRONTEND=/home/erikg/sxgc/vendor/chunk-merge-v3/chunk_frontend_buffered \
CHUNK_BCR_MERGE="$artifact/chunk_bcr_merge" \
    python3 bit6/sxi_logs/chunk-merge-v3/test_chunk_bcr_merge.py -v \
    > "$journal/unit-gate.log" 2>&1
printf 'TREE_RUNNING pid=%s utc=%s\n' "$$" "$(date -u +%FT%TZ)" > "$journal/pipeline.status"
trap 'rc=$?; printf "TREE_EXIT code=%s pid=%s utc=%s\n" "$rc" "$$" "$(date -u +%FT%TZ)" >> "$journal/pipeline.status"' EXIT
/usr/bin/time -v -o "$journal/tree.time" \
    "$artifact/chunk_bcr_merge" --tree "$chunks" 16 1082130213 \
    "$artifact/frag" "$artifact/states" 8 > "$journal/tree.log" 2>&1
bash "$journal/finish_gate.sh" > "$journal/finish-gate.log" 2>&1
python3 "$journal/summarize.py" > "$journal/summarize.log" 2>&1
printf 'TREE_ALL_GATES_PASS utc=%s\n' "$(date -u +%FT%TZ)" >> "$journal/pipeline.status"
