#!/usr/bin/env bash
# Full verified 1 GB stream from bounded precomputed columns, not a finish gate.
set -euo pipefail
T=$(mktemp -d /tmp/all-rung1-XXXXXX)
for f in overlap_macro overlap_list_join overlap_spill_emit overlap_prefix_oracle;do g++ -O2 -std=c++17 -Wall -Wextra -Werror gates/$f.cpp -o "$T/$f";done
b=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;D=$(mktemp -d "$b/all-rung1-XXXXXX");echo receipt=$D; mkdir "$D/in"
cp --reflink=never "$b/macro-repair-UljOUZ/"{corpus,physical.refs} "$b/macro-X85IfY/work/cells.bin" "$b/occ-lists-7BmhUN/work/"{loc.u32,pos-next.u32x2,pred.u8} "$b/aligned-qydfUW/work/aligned.u32" "$D/in/"
cp --reflink=never "$b/macro-repair-UljOUZ/exact.plan" "$D/in/plan"; cp --reflink=never "$D/in/corpus" "$D/oracle-corpus"; python3 gates/private_cold.py "$D/in/"* "$D/oracle-corpus"
systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 /usr/bin/python3 "$PWD/gates/overlap_all_sets.py" "$D" "$T" 2>"$D/run.log" | systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=1400M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$T/overlap_prefix_oracle" "$D/oracle-corpus" >"$D/oracle.log" 2>"$D/oracle.err"
tail -1 "$D/run.log";tail -1 "$D/oracle.log";grep -q '^ORACLE rows=1000000666 ' "$D/oracle.log"
