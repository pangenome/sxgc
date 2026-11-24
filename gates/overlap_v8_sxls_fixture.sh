#!/bin/bash
# Bounded generated fixed SXLS codec, handoff and partial-input refusals. No corpus/finish service.
set -euo pipefail
cd "$(dirname "$0")/..";d=${1:?private fixture directory};[[ $# == 1 && $d == /tmp/v8-sxls-slots-* && -O $d && $d == "$(realpath -- "$d")" && $(stat -c %a "$d") == 700 ]]
cg=/sys/fs/cgroup$(awk -F'::' '$1=="0" {print $2}' /proc/self/cgroup)
[[ $(<"$cg/memory.max") == 805306368 && $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '100000 100000' ]]
umask 077;mkdir "$d/build" "$d/tmp" "$d/partial";export TMPDIR=$d/tmp CARGO_TARGET_DIR=$d/build CARGO_BUILD_JOBS=1 CARGO_INCREMENTAL=0 CARGO_PROFILE_TEST_DEBUG=0 CARGO_PROFILE_DEV_DEBUG=0 PATH="$HOME/.cargo/bin:/usr/bin:/bin"
a=(--manifest-path xsa/Cargo.toml --locked --offline -p pfp --bin pfp_sort_r)
cargo test "${a[@]}" unit::v8_sxls::tests -- --test-threads=1
cargo test "${a[@]}" unit::v8_piece_pool_test -- --test-threads=1
cargo test "${a[@]}" unit::handoff_test -- --test-threads=1
cargo test "${a[@]}" escape_publications_are_byte_identical -- --test-threads=1
cargo build "${a[@]}";exe=$CARGO_TARGET_DIR/debug/pfp_sort_r
python3 -BE gates/overlap_v8_piece_pool_refuse.py "$exe" "$TMPDIR"
python3 -BE gates/overlap_v8_sxls_refuse.py "$exe" "$d/partial"
sha256sum "$exe";python3 -BE gates/overlap_finish_credits.py review | cmp - gates/receipts/finish-credits-100m/review.json
