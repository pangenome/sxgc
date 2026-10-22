#!/usr/bin/env bash
# entropy_ab.sh — THE ENTROPY RULE A/B (tools/binbwt/ENTROPY_RULE_DESIGN.md §7).
#
#   bash tools/binbwt/entropy_ab.sh SIZE ROOT [threads] [extra producer flags...]
#
# Runs the producer's CONSTRUCTION-ONLY probe (`--bin-probe`: the same counters
# and walls, no sink, no output) once per arm on the SAME corpus/remap, one arm
# at a time, and prints per arm the fields the report is built from together
# with the box's load average. `build_cpu_s` is the honest number on a busy box;
# `build_s` is the wall and is labelled with what shared the machine.
#
# Arms (each with the in-leaf arm's row floor at the product default, 16):
#   off      today's target-size rule (the reference, rule unarmed)
#   e<b>     --bin-entropy-min b: tree theta = b, in-leaf theta = b*0.25
#   t<b>     tree arm only: BIN_ENTROPY_LEAF_MIN=1e9 disables the in-leaf arm
#   l<b>     in-leaf arm only: tree theta = b, in-leaf theta = 0 and floor 1
#            (the in-leaf lever priced on its own)
#
# Every scratch directory is fresh; every log is kept under $ROOT.
set -uo pipefail
cd "$(dirname "$0")/../.."
SIZE="${1:?usage: entropy_ab.sh SIZE ROOT [threads] [flags...]}"
ROOT="${2:?}"
THREADS="${3:-32}"
shift $(( $# < 3 ? $# : 3 ))
EXTRA=("$@")
CORPUS="${CORPUS:-/mnt/nvme2n1/erikg/extcols-scratch/mergperf-${SIZE}-snap.txt}"
REMAP="${REMAP:-/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin}"
BIN="${BINBWT_BIN:-/mnt/nvme2n1/erikg/er2-build/binbwt}"
mkdir -p "$ROOT"
say() { echo "### $* (loadavg $(cut -d' ' -f1 /proc/loadavg) $(date +%T))"; }
run() {           # run TAG [producer flags...]
    local tag="$1"; shift
    local dir="$ROOT/win-$tag"
    rm -rf "$dir"; mkdir -p "$dir"
    local load0; load0=$(cut -d' ' -f1 /proc/loadavg)
    echo "== arm $tag loadavg=$load0"
    env CROSS_POOL_SLOTS=33554432 "$BIN" --binbwt "$CORPUS" --remap "$REMAP" \
        --bin-probe --threads "$THREADS" --bin-window-dir "$dir" "${EXTRA[@]}" "$@" \
        > "$ROOT/$tag.log" 2>&1
    local rc=$?
    {
      grep -a '^BIN_WORK ' "$ROOT/$tag.log" | tail -1 | tr ' ' '\n' \
        | grep -E '^(build_s|build_cpu_s|leaf_s|level_passes|level_elems|bin_passes|comparisons|symbols|leaf_sorts|leaf_elems|emitted_rows|depth_max|leaf_rows_max|entropy_armed|entropy_min_bits|entropy_leaf_min_bits|entropy_evals|entropy_taken|entropy_exits|entropy_exit_rows|entropy_noprog_exits|entropy_leaf_evals|entropy_leaf_taken|entropy_leaf_exits|entropy_leaf_rows|entropy_over_target_leaves|win_file_bytes|stage_bytes)='
      grep -a '^BIN_HOT ' "$ROOT/$tag.log" | tail -1 \
        | grep -oE '(bin|rows|cpu_s|level_elems|comparisons|symbols|leaf_sorts)=[0-9.]+' \
        | sed 's/^/hot_/'
      echo "rc=$rc loadavg_start=$load0"
    } | tee "$ROOT/$tag.arm"
    return $rc
}
say "AB size=$SIZE threads=$THREADS extra=${EXTRA[*]:-none}"
run off
for b in ${ENTROPY_BITS:-0.75 1.5 3}; do run "e$b" --bin-entropy-min "$b"; done
for b in ${ENTROPY_TREE_BITS:-1.5}; do
    export BIN_ENTROPY_LEAF_MIN=1000000000
    run "t$b" --bin-entropy-min "$b"
    unset BIN_ENTROPY_LEAF_MIN
done
for b in ${ENTROPY_LEAF_ARM_BITS:-}; do
    export BIN_ENTROPY_LEAF_MIN=0 BIN_ENTROPY_LEAF_FLOOR=1
    run "l$b" --bin-entropy-min "$b"
    unset BIN_ENTROPY_LEAF_MIN BIN_ENTROPY_LEAF_FLOOR
done
say "AB_DONE $SIZE"
