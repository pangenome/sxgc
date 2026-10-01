#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
journal=$root/bit6/sxi_logs/merge-tree
merged=/home/erikg/sxgc/vendor/chunk-merge-tree/frag
reference=/home/erikg/sxgc/vendor/chunk-merge-v3/reference/parse
tools_dir=/home/erikg/sxgc/sealed-tools-v2
xsa=/home/erikg/sxgc/xsa/target/release/xsa
for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do
    cmp "${merged}${ext}" "${reference}${ext}"
    printf 'CMP_PASS %s\n' "$ext"
done > "$journal/four-file-gate.log"
/usr/bin/time -v -o "$journal/endpoints.time" \
    "$tools_dir/rpfbwt_endpoints" "$merged" \
    /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.ri4 \
    /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.head_sa 1e --w1 10 \
    > "$journal/endpoints.log" 2>&1
/usr/bin/time -v -o "$journal/slim.time" \
    "$tools_dir/slim_dump" --slim --resolve-ri4 --dict-stream \
    --ri4 /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.ri4 \
    --head-sa /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.head_sa \
    --parse "$reference" --w1 10 -t 8 \
    -o /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.agg \
    > "$journal/slim.log" 2>&1
/usr/bin/time -v -o "$journal/chi.time" \
    "$xsa" chi-rspace --stream-agg \
    --ri4 /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.ri4 \
    --agg /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.agg \
    -o /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.sA \
    > "$journal/chi.log" 2>&1
rg -F 'chi = 306164765 (N=1082130213, R=397723010)' "$journal/chi.log" \
    > "$journal/chi-gate.log"
test "$(stat -c %s /home/erikg/sxgc/vendor/chunk-merge-tree/fresh.sA)" = 2449318120
printf 'CHI_PASS count=306164765 bytes=2449318120\n' >> "$journal/chi-gate.log"
printf 'FINISH_GATE_PASS\n'
