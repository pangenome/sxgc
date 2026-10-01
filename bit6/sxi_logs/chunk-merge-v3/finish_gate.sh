#!/usr/bin/env bash
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$root"
journal=bit6/sxi_logs/chunk-merge-v3
merged=vendor/chunk-merge-v3/merged/frag
reference=vendor/chunk-merge-v3/reference/parse
tools_dir=/home/erikg/sxgc/sealed-tools-v2
xsa=/home/erikg/sxgc/xsa/target/release/xsa

for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do
    test -s "${merged}${ext}"
    test -s "${reference}${ext}"
    cmp "${merged}${ext}" "${reference}${ext}"
    printf 'CMP_PASS %s\n' "$ext"
done > "$journal/four-file-gate.log"

/usr/bin/time -v -o "$journal/merged-endpoints.time" \
    "$tools_dir/rpfbwt_endpoints" "$merged" \
    vendor/chunk-merge-v3/merged/fresh.ri4 \
    vendor/chunk-merge-v3/merged/fresh.head_sa 1e --w1 10 \
    > "$journal/merged-endpoints.log" 2>&1

/usr/bin/time -v -o "$journal/merged-slim.time" \
    "$tools_dir/slim_dump" --slim --resolve-ri4 --dict-stream \
    --ri4 vendor/chunk-merge-v3/merged/fresh.ri4 \
    --head-sa vendor/chunk-merge-v3/merged/fresh.head_sa \
    --parse "$reference" --w1 10 -t 8 \
    -o vendor/chunk-merge-v3/merged/fresh.agg \
    > "$journal/merged-slim.log" 2>&1

/usr/bin/time -v -o "$journal/merged-chi.time" \
    "$xsa" chi-rspace --stream-agg \
    --ri4 vendor/chunk-merge-v3/merged/fresh.ri4 \
    --agg vendor/chunk-merge-v3/merged/fresh.agg \
    -o vendor/chunk-merge-v3/merged/fresh.sA \
    > "$journal/merged-chi.log" 2>&1

rg -F 'chi = 306164765 (N=1082130213, R=397723010)' "$journal/merged-chi.log" \
    > "$journal/chi-gate.log"
test "$(stat -c %s vendor/chunk-merge-v3/merged/fresh.sA)" = 2449318120
printf 'CHI_PASS count=306164765 bytes=2449318120\n' >> "$journal/chi-gate.log"
sha256sum "${merged}.rlebwt" "${merged}.rlebwt.meta" \
    "${merged}.ssa" "${merged}.ssa_t" > "$journal/merged-hashes.txt"
printf 'FINISH_GATE_PASS\n'
