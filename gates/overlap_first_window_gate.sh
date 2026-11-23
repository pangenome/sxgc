#!/usr/bin/env bash
set -eu
T=$(mktemp -d /tmp/window-XXXXXX)
for f in overlap_window overlap_window_oracle; do g++ -O2 -std=c++17 -Wall -Wextra -Werror gates/$f.cpp -o "$T/$f"; done
base=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
for spec in mffG5V:mQzciL:hi1pEq 81tu3m:7BmhUN:qydfUW; do
 IFS=: read -r t o a <<< "$spec"; D=$(mktemp -d "$base/first-window-XXXXXX"); echo receipt=$D; mkdir "$D/in"
 cp --reflink=never "$base/emit-tape-$t/work/"{cells.bin,words.tape} "$base/occ-lists-$o/work/"{loc.u32,pos-next.u32x2,pred.u8} "$base/aligned-$a/work/aligned.u32" "$base/emit-tape-$t/corpus" "$D/in/"
 python3 gates/private_cold.py "$D/in/"*
 systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$T/overlap_window" "$D/in" "$D/in" "$D/in/aligned.u32" "$D/in/corpus" "$D/triples" 1100000 >"$D/run.log"
 tail -1 "$D/run.log"; hi=$(awk -F'[ =]' '/WINDOW/{print $3}' "$D/run.log")
 systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=1400M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$T/overlap_window_oracle" "$D/in/corpus" "$D/triples" "$hi" >"$D/oracle.log"; tail -1 "$D/oracle.log"
done
