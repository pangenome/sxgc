#!/bin/bash
# One operator-requested rung. Never overlap builds or repeat a prepared arm.
set -euo pipefail;cd "$(dirname "$0")/..";[[ $# == 1 ]] || exit 64;r=$1
case $r in 1g) gb=8;lease=720;;10g) gb=48;lease=2400;;100g) gb=220;lease=21600;;*) exit 64;;esac
[[ $(git rev-parse HEAD) == "$(git rev-parse origin/main)" ]] || exit 75;source gates/overlap_pin.sh;overlap_pin_series gates/rolling_ladder_prepare.sh "$r"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;for old in "$s"/rolling-build-"$r"-*;do [[ ! -e $old ]] || { echo "inspect prior run: $old";exit 75; };done
python3 -BE -c 'import sys;sys.path.insert(0,"gates");from rolling_ladder import promotion;promotion(sys.argv[1])' "$r"
umask 077;d=$(mktemp -d "$s/rolling-build-$r-XXXXXX");echo "ROLLING_BUILD=$d";echo "$SXGC_PINNED_SOURCE" >"$d/prepare-source.log";trap 'rc=$?;echo prepare_launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
cores=$(python3 -BE -c 'import sys;sys.path.insert(0,"gates");from overlap_parse64_study import cores;print(",".join(map(str,cores()[:4])))');end=$(( $(date +%s)+lease ));rc=0
systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p "MemoryMax=${gb}G" -p MemorySwapMax=0 -p CPUQuota=400% -p IOAccounting=yes -p Nice=10 -p LimitCORE=0 -p LimitSTACK=8M -p UMask=0077 -p "RuntimeMaxSec=$((lease+6))s" -p TimeoutStopSec=5s taskset -c "$cores" /usr/bin/python3 -BE "$PWD/gates/rolling_ladder_prepare.py" "$r" "$d" "$end" >"$d/prepare.log" 2>&1 || rc=$?;echo prepare=$rc >>"$d/exits.log";exit "$rc"
