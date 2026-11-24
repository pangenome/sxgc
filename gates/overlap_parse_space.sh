#!/usr/bin/env bash
# Fast diagnostic, no large-corpus run or BWT merge. Separate from product timings.
set -euo pipefail
cd "$(dirname "$0")/.."; S=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
D=$(mktemp -d "$S/parse-space-XXXXXX"); echo receipt="$D"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_parse_space.cpp -o "$D/test"
cp --reflink=never "$S/dict-adj-kIPyZL/columns/parse.u32" "$S/parse-ranks-Ys27jk/work/inverse.u32" "$D/"
python3 gates/private_cold.py "$D/parse.u32" "$D/inverse.u32"
systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=1G -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 /usr/bin/time -v "$D/test" "$D/parse.u32" "$D/inverse.u32" > "$D/result.log" 2> "$D/time.log"
tail -n 2 "$D/result.log"
