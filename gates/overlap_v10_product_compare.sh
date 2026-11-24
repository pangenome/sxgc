#!/bin/bash
# Generated, fixed 6,311,445 byte input ONLY. The two sides make real v10 containers.
set -euo pipefail
first=reference
if [[ $# == 1 && $1 == --check-joined-reference ]]; then first=check-joined-reference; else [[ $# == 0 ]]; fi
cd "$(dirname "$0")/.."; umask 077
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
python3 -BE gates/overlap_timing_idle.py
[[ $(df --output=avail -B1 /tmp | tail -1) -gt 536870912 ]]
d=$(mktemp -d /tmp/rolling-v10-product-XXXXXX);echo "FULL_PRODUCT=$d"
trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
cores=$(python3 -BE -c 'import sys;sys.path.insert(0,"gates");from overlap_parse64_study import cores;print(",".join(map(str,cores()[:2])))')
u=(systemd-run --user --wait --pipe --quiet --collect --expand-environment=no -p MemorySwapMax=0 -p CPUQuota=200% -p TimeoutStopSec=4s -p LimitCORE=0 -p Nice=10 -p IOAccounting=yes -p UMask=0077)
phase(){ local name=$1 rc=0;shift; "$@" >"$d/$name.log" 2>&1 || rc=$?; echo "$name=$rc" >>"$d/exits.log"; return "$rc"; }
git rev-parse HEAD >"$d/base";sha256sum gates/{overlap_v10_product_{compare.py,compare.sh,input.py,pipe.hpp},overlap_row43_oracle.cpp,private_cold.py} >"$d/sources.sha"
phase compile "${u[@]}" -p MemoryMax=256M -p RuntimeMaxSec=40s -p LimitFSIZE=1M taskset -c "$cores" /usr/bin/g++ -std=c++17 -O2 -Wall -Wextra -Werror "$PWD/gates/overlap_row43_oracle.cpp" -o "$d/witness"
phase pins /usr/bin/python3 -BE gates/overlap_v10_product_compare.py pins
sha256sum "$d/witness" >"$d/witness.sha"
for arm in "$first" candidate;do
 echo "START=$arm"
 phase "arm-$arm" "${u[@]}" -p MemoryMax=384M -p RuntimeMaxSec=150s -p LimitFSIZE=16M -p LimitSTACK=8M taskset -c "$cores" /usr/bin/python3 -BE "$PWD/gates/overlap_v10_product_compare.py" "$arm" "$d"
done
sha256sum -c "$d/sources.sha";echo "COMPLETE=$d"
