#!/bin/bash
# Only continue the already-ranked rung: no rebuilding the front or overwriting
# a partial. Rungs are serial; the next preparation checks a banked predecessor.
set -euo pipefail;cd "$(dirname "$0")/..";[[ $# == 2 ]] || exit 64;r=$1;d=$2;s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
case $r in 1g) gb=8;lease=4200;;10g) gb=48;lease=36000;;100g) gb=220;lease=345600;;*) exit 64;;esac
[[ $d == "$s/rolling-build-$r-"?????? && $d == "$(realpath "$d")" && -f $d/prepared.json && ! -e $d/stop.requested && ! -e $d/product-source.log ]] || exit 75
[[ $(git rev-parse HEAD) == "$(git rev-parse origin/main)" ]] || exit 75;source gates/overlap_pin.sh;overlap_pin_series gates/rolling_ladder_finish.sh "$r" "$d"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
umask 077;echo "CONTINUE_ROLLING=$d";echo "$SXGC_PINNED_SOURCE" >"$d/product-source.log";trap 'rc=$?;echo product_launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
cores=$(python3 -BE -c 'import sys;sys.path.insert(0,"gates");from overlap_parse64_study import cores;print(",".join(map(str,cores()[:4])))');end=$(( $(date +%s)+lease ));rc=0
systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p "MemoryMax=${gb}G" -p MemorySwapMax=0 -p CPUQuota=400% -p IOAccounting=yes -p Nice=10 -p LimitCORE=0 -p LimitSTACK=8M -p UMask=0077 -p "RuntimeMaxSec=$((lease+10))s" -p TimeoutStopSec=5s taskset -c "$cores" /usr/bin/python3 -BE "$PWD/gates/rolling_ladder_finish.py" "$r" "$d" "$end" >"$d/product.log" 2>&1 || rc=$?;echo product=$rc >>"$d/exits.log";exit "$rc"
