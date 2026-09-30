#!/usr/bin/env bash
set -euo pipefail
cd /home/erikg/worktrees/sxgc/pi-worktree-2ec315bd-0a1b-4034-bd8d-f196af8293a7-s0-0
log=bit6/sxi_logs/bcr-v2
scratch=/tmp/bcr-v2-gates-2ec315bd
mkdir -p "$scratch"
binary=/tmp/bcr_frontend_v2
yeast_source=/tmp/sxi-repair2/yeast235/xsa-build-26al38ee/collection.txt
yeast_ref=/tmp/sxi-repair2/yeast235/xsa-build-26al38ee/parse
frag_source=/home/erikg/sxgc-piletest/pile-frag.txt
frag_ref=/home/erikg/worktrees/sxgc/pi-worktree-e7286239-b261-424c-8585-f2604e5d83e0-s0-0/vendor/byte-remap-gates/xsa-build-_69d9g2j/parse
run_gate() {
    local name=$1 source=$2 reference=$3
    shift 3
    local out="$scratch/$name"
    printf 'START %s %s\n' "$name" "$(date -u +%FT%TZ)" >> "$log/scale-gates.log"
    /usr/bin/time -v -o "$log/$name.time" "$binary" "$source" "$out" "$@" >> "$log/scale-gates.log" 2>&1
    for suffix in .rlebwt .rlebwt.meta .ssa .ssa_t; do
        cmp "$out$suffix" "$reference$suffix"
        printf 'PASS %s%s byte-identical\n' "$name" "$suffix" >> "$log/scale-gates.log"
    done
    printf 'DONE %s %s\n' "$name" "$(date -u +%FT%TZ)" >> "$log/scale-gates.log"
}
run_gate yeast235 "$yeast_source" "$yeast_ref"
run_gate pile-frag "$frag_source" "$frag_ref" --remap "$frag_ref.remap"
