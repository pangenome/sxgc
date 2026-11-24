#!/usr/bin/env bash
# Component study only. DEADLINE is absolute Unix epoch seconds.
set -euo pipefail
[[ $# == 3 && $3 =~ ^[1-9][0-9]{0,10}$ ]] || { echo 'usage: overlap_ten_dict_grid.sh RECEIPT10 PAIR1 DEADLINE' >&2; exit 64; }
ten=$(realpath "$1"); pair=$(realpath "$2"); deadline=$3
cd "$(dirname "$0")/.."; repo=$PWD; s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
remaining() { local n=$((deadline-$(date +%s))); ((n>0)) || return 75; echo "$n"; }
remaining >/dev/null
exec 9>"$s/.overlap-ten-dict-grid.lock"; flock -n 9 || exit 75
py=(/usr/bin/python3 -E "$repo/gates/overlap_ten_dict_grid.py")
"${py[@]}" preflight "$ten" "$pair" "$deadline"
d=$(mktemp -d "$s/dict-grid-10g-XXXXXX"); echo "component receipt=$d"
# Independent target; never replace a live product producer.
n=$(remaining); timeout -k 1 "$n" env XSA_BUILD_DICT_OPENMP=1 CC=gcc cargo build -j1 --release --locked --offline --features dict-grid --manifest-path xsa/pfp/Cargo.toml --bin overlap_resident --target-dir "$d/target" >"$d/build.log" 2>&1
exe=$d/target/release/overlap_resident
{ git rev-parse HEAD; git status --porcelain; sha256sum "$exe"; gcc --version; } >"$d/binaries.log"
n=$(remaining); timeout -k 1 "$n" /usr/bin/python3 -E gates/overlap_ten_dict_grid_test.py "$exe" >"$d/fixtures.log"
n=$(remaining); systemd-run --user --quiet --collect --wait --pipe -p RuntimeMaxSec="$n" -p TimeoutStopSec=1 -p MemoryMax=200G -p MemorySwapMax=0 -p CPUQuota=400% -p Nice=10 "${py[@]}" worker "$d" "$exe" "$ten" "$pair" "$deadline"
echo "completed component receipt=$d"
