#!/bin/bash
# Synthetic files and refusal paths only. No real row producer, sink+finish arm or timing.
set -euo pipefail
cd "$(dirname "$0")/.."; d=${1:?private dir}
[[ $d == /tmp/v8-piece-slots-* && $d == "$(realpath -- "$d")" && -d $d && -O $d && $(stat -c %a "$d") == 700 ]]
cg=/sys/fs/cgroup$(awk -F'::' '$1 == "0" {print $2}' /proc/self/cgroup)
[[ $(<"$cg/memory.max") == 805306368 && $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '100000 100000' ]]
export CARGO_TARGET_DIR=$d/build TMPDIR=$d/tmp CARGO_BUILD_JOBS=1 CARGO_INCREMENTAL=0 CARGO_PROFILE_TEST_DEBUG=0 CARGO_PROFILE_DEV_DEBUG=0
umask 077; mkdir "$CARGO_TARGET_DIR" "$TMPDIR"
export PATH="$HOME/.cargo/bin:/usr/bin:/bin"
a=(--manifest-path xsa/Cargo.toml --locked --offline -p pfp --bin pfp_sort_r)
cargo test "${a[@]}" unit::v8_piece_pool_test -- --test-threads=1
cargo test "${a[@]}" unit::handoff_test -- --test-threads=1
cargo test "${a[@]}" escape_publications_are_byte_identical -- --test-threads=1
cargo build "${a[@]}"
python3 -BE gates/overlap_v8_piece_pool_refuse.py "$CARGO_TARGET_DIR/debug/pfp_sort_r" "$TMPDIR"
sha256sum "$CARGO_TARGET_DIR/debug/pfp_sort_r"
