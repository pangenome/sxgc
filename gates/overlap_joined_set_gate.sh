#!/usr/bin/env bash
set -euo pipefail
T=$(mktemp -d /tmp/join-set-XXXXXX)
for f in overlap_list_join overlap_spill_emit overlap_prefix_oracle;do g++ -O2 -std=c++17 -Wall -Wextra -Werror gates/$f.cpp -o "$T/$f";done
base=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
for spec in macro-MdCk5Y:plan:mQzciL:hi1pEq:6 macro-repair-UljOUZ:exact.plan:7BmhUN:qydfUW:66;do
 IFS=: read -r mac plan occ al max <<< "$spec";D=$(mktemp -d "$base/joined-set-XXXXXX");echo receipt=$D;mkdir "$D/in" "$D/spill";cp --reflink=never "$base/$mac/corpus" "$base/occ-lists-$occ/work/"{loc.u32,pos-next.u32x2,pred.u8} "$base/aligned-$al/work/aligned.u32" "$D/in/";cp --reflink=never "$base/$mac/$plan" "$D/plan";cp --reflink=never "$D/in/corpus" "$D/oracle-corpus";n=$(stat -c%s "$D/in/corpus")
 read -r limit live < <(python3 - "$D" "$max" <<'PY'
import sys,struct,pathlib
p=pathlib.Path(sys.argv[1]);sz=lambda f:(p/f).stat().st_size;n=sz('in/corpus');k=sz('in/loc.u32')//4-1;live=k*16+16*258**2+sz('plan')+sum(sz('in/'+x) for x in ['aligned.u32','loc.u32','pos-next.u32x2','pred.u8']); reserve=0;cnt=0
with (p/'plan').open('rb') as f:
 f.read(4)
 for _ in range(int(sys.argv[2])):
  d,b,r,t=struct.unpack('<QQQI',f.read(28));f.seek(t&0x7fffffff,1);key=16*d+b;extra=12*d+9*r
  if live+reserve+key+extra>2*n:break
  live+=key;reserve+=extra;cnt+=1
assert cnt;print(cnt,live)
PY
 )
 for ((i=0;i<limit;i++));do cp --reflink=never "$base/$mac/spill/$i" "$D/spill/$i";done
 python3 gates/private_cold.py "$D/in/"* "$D/spill/"* "$D/plan" "$D/oracle-corpus"
 run(){ systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$@"; }
 run "$T/overlap_list_join" "$D/in" "$D/spill" "$D/side" "$limit" "$n" "$live" >"$D/join.log"; tail -1 "$D/join.log"
 python3 gates/private_cold.py "$D/spill/"* "$D/side/"* "$D/in/aligned.u32"
 run "$T/overlap_spill_emit" "$D/in" "$D/spill" "$D/plan" "$limit" "$n" "$D/side" 2>"$D/run.log" | systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=1400M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 "$T/overlap_prefix_oracle" "$D/oracle-corpus" >"$D/oracle.log" 2>"$D/oracle.err"
 tail -1 "$D/run.log";tail -1 "$D/oracle.log"
done
