#!/bin/bash
# One partial 100 MB real-rank probe. No 100 GB, all-row, builder or clock rerun.
set -euo pipefail;cd "$(dirname "$0")/..";now=$(date +%s)
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1>now+60 && $1<=now+180 )) || exit 75
end=$1;source gates/overlap_pin.sh;overlap_pin_series gates/overlap_prefix_real_windows.sh "$end"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;for q in "$s"/raw-prefix-windows-*;do [[ ! -e $q ]] || { echo "HOLD: inspect prior window probe $q";exit 75; };done
umask 077;d=$(mktemp -d "$s/raw-prefix-windows-XXXXXX");echo "RANK_WINDOW_PROBE=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
echo "$SXGC_PINNED_SOURCE" >"$d/source.log";one=$(python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');now=$(date +%s);((end>now+55)) || exit 75
rc=0;systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=100% -p MemoryMax=256M -p MemorySwapMax=0 -p IOAccounting=yes -p Nice=10 -p LimitCORE=0 -p LimitFSIZE=101M -p LimitSTACK=8M -p UMask=0077 -p "RuntimeMaxSec=$((end-now+2))s" -p TimeoutStopSec=3s taskset -c "$one" /usr/bin/python3 -BE "$PWD/gates/overlap_prefix_real_windows.py" "$d" "$end" >"$d/review.log" 2>&1 || rc=$?;echo "review=$rc" >>"$d/exits.log";exit "$rc"
