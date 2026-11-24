#!/usr/bin/env bash
# Each arm uses a fresh private inode set. Producer emits only to pipe;
# verifier RAM and corpus cache are separate, not charged to producer.
set -euo pipefail
T=$(mktemp -d /tmp/emit-set-XXXXXX)
for f in overlap_spill_emit overlap_prefix_oracle;do g++ -O2 -std=c++17 -Wall -Wextra -Werror gates/$f.cpp -o "$T/$f";done
base=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
for spec in macro-MdCk5Y:plan:mQzciL:hi1pEq:6 macro-repair-UljOUZ:exact.plan:7BmhUN:qydfUW:66;do
 IFS=: read -r mac plan occ al limit <<< "$spec";D=$(mktemp -d "$base/set-emit-XXXXXX");echo receipt=$D;mkdir "$D/in" "$D/spill";cp --reflink=never "$base/$mac/spill/"* "$D/spill/";cp --reflink=never "$base/$mac/corpus" "$base/occ-lists-$occ/work/"{loc.u32,pos-next.u32x2,pred.u8} "$base/aligned-$al/work/aligned.u32" "$D/in/";cp --reflink=never "$base/$mac/$plan" "$D/plan";cp --reflink=never "$D/in/corpus" "$D/oracle-corpus";python3 gates/private_cold.py "$D/in/"* "$D/spill/"* "$D/plan" "$D/oracle-corpus"
 systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$T/overlap_spill_emit" "$D/in" "$D/spill" "$D/plan" "$limit" "$(stat -c%s "$D/in/corpus")" 2>"$D/run.log" | systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=1400M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$T/overlap_prefix_oracle" "$D/oracle-corpus" >"$D/oracle.log" 2>"$D/oracle.err"
 tail -1 "$D/run.log";tail -1 "$D/oracle.log"
done
