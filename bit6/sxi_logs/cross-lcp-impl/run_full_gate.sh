#!/usr/bin/env bash
# Cross-LCP merge full gate (16-chunk Pile fragment).
#   stage-tree    : pairwise tree merge over the 16 banked SXCR chunks
#                   -> merged four files -> byte cmp vs banked BCR output
#   stage-serial  : serial fold (cost-table sibling), byte cmp as redundancy
#   stage-finish  : banked finish sequence (endpoints/slim/chi) on the tree
#                   output, with the reference parse symlink trick
#   stage-cost    : COST_TABLE.md from the journaled CROSS_PAIR rows
# Usage: run_full_gate.sh <stage> [...]
set -euo pipefail

root=$(cd "$(dirname "$0")/../../.." && pwd)
journal=$root/bit6/sxi_logs/cross-lcp-impl
run=${CROSS_LCP_RUN:-/home/erikg/cross-lcp-run}
chunks=/home/erikg/sxgc/vendor/chunk-merge-v3/chunks
total=1082130213
banked=/home/erikg/sxgc/vendor/chunk-merge-v3/merged
ref=/home/erikg/sxgc/vendor/chunk-merge-v3/reference
tools=/home/erikg/sxgc/sealed-tools-v2
xsa=/home/erikg/sxgc/xsa/target/release/xsa
cross=${CROSS_LCP_MERGE:-/tmp/cross_lcp_merge}

stage_tree() {
    mkdir -p "$run/merged"
    /usr/bin/time -v "$cross" "$chunks" 16 "$total" "$run/merged/frag" \
        --work "$run/tree-work" --threads 8 \
        > "$journal/merge-tree.log" 2> "$journal/merge-tree.err"
    {
        for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do
            test -s "${run}/merged/frag${ext}"
            test -s "${banked}/frag${ext}"
            cmp "${run}/merged/frag${ext}" "${banked}/frag${ext}"
            printf 'CMP_PASS %s\n' "$ext"
        done
        printf 'FOUR_FILE_GATE_PASS\n'
    } > "$journal/four-file-gate.log"
    sha256sum "${run}/merged/frag.rlebwt" "${run}/merged/frag.rlebwt.meta" \
        "${run}/merged/frag.ssa" "${run}/merged/frag.ssa_t" > "$journal/merged-hashes.txt"
    cp "$journal/merge-tree.err" "$journal/merge-tree.time"
}

stage_serial() {
    mkdir -p "$run/serial"
    /usr/bin/time -v "$cross" "$chunks" 16 "$total" "$run/serial/frag" \
        --work "$run/serial-work" --serial --threads 8 \
        > "$journal/merge-serial.log" 2> "$journal/merge-serial.err"
    {
        for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do
            test -s "${run}/serial/frag${ext}"
            cmp "${run}/serial/frag${ext}" "${banked}/frag${ext}"
            printf 'CMP_PASS serial %s\n' "$ext"
        done
        printf 'SERIAL_FOUR_FILE_GATE_PASS\n'
    } > "$journal/serial-four-file-gate.log"
    cp "$journal/merge-serial.err" "$journal/merge-serial.time"
}

stage_finish() {
    # The endpoints/slim/chi chain needs <prefix>.{dict,parse,parse.dict,parse.parse,remap};
    # symlink the read-only reference parse files (the banked dictionary trick).
    for stem in dict parse parse.dict parse.parse remap; do
        ln -sfn "$ref/parse.$stem" "$run/merged/frag.$stem"
    done
    /usr/bin/time -v "$tools/rpfbwt_endpoints" "$run/merged/frag" \
        "$run/merged/fresh.ri4" "$run/merged/fresh.head_sa" 1e --w1 10 \
        > "$journal/merged-endpoints.log" 2> "$journal/merged-endpoints.err"
    cp "$journal/merged-endpoints.err" "$journal/merged-endpoints.time"
    /usr/bin/time -v "$tools/slim_dump" --slim --resolve-ri4 --dict-stream \
        --ri4 "$run/merged/fresh.ri4" --head-sa "$run/merged/fresh.head_sa" \
        --parse "$ref/parse" --w1 10 -t 8 \
        -o "$run/merged/fresh.agg" \
        > "$journal/merged-slim.log" 2> "$journal/merged-slim.err"
    cp "$journal/merged-slim.err" "$journal/merged-slim.time"
    /usr/bin/time -v "$xsa" chi-rspace --stream-agg \
        --ri4 "$run/merged/fresh.ri4" --agg "$run/merged/fresh.agg" \
        -o "$run/merged/fresh.sA" \
        > "$journal/merged-chi.log" 2> "$journal/merged-chi.err"
    cp "$journal/merged-chi.err" "$journal/merged-chi.time"
    cat "$journal/merged-chi.log" "$journal/merged-chi.err" > "$journal/merged-chi.combined" || true
    rg -F 'chi = 306164765 (N=1082130213, R=397723010)' "$journal/merged-chi.combined" \
        > "$journal/chi-gate.log"
    test "$(stat -c %s "$run/merged/fresh.sA")" = 2449318120
    printf 'CHI_PASS count=306164765 bytes=2449318120\n' >> "$journal/chi-gate.log"
    cat "$journal/chi-gate.log"
}

stage_cost() {
    python3 "$journal/make_cost_table.py" \
        > "$journal/COST_TABLE.md"
    cat "$journal/COST_TABLE.md"
}

for stage in "$@"; do
    case "$stage" in
        tree) stage_tree ;;
        serial) stage_serial ;;
        finish) stage_finish ;;
        cost) stage_cost ;;
        *) echo "unknown stage $stage" >&2; exit 1 ;;
    esac
done
