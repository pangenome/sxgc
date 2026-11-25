#!/bin/bash
# Fixed, read-only m12 geometry. No full payload/row validation, no corpus or construction workers.
set -euo pipefail
cd "$(dirname "$0")/..";now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1>now+55 && $1<=now+120 )) || exit 75
end=$1;source gates/overlap_pin.sh;overlap_pin_series gates/overlap_v10_target_credits.sh "$end"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
python3 -BE gates/overlap_timing_idle.py;s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
for q in "$s"/v10-target-credits-*;do [[ ! -e $q ]] || { echo "HOLD: preserve prior metadata study: $q";exit 75; };done
umask 077;d=$(mktemp -d "$s/v10-target-credits-XXXXXX");echo "CREDIT_REVIEW=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
one=$(python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');now=$(date +%s);((end>now+50)) || exit 75
echo "$SXGC_PINNED_SOURCE" >"$d/source.log";rc=0;systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=100% -p MemoryMax=128M -p MemorySwapMax=0 -p IOAccounting=yes -p Nice=10 -p LimitCORE=0 -p LimitFSIZE=1M -p LimitSTACK=8M -p UMask=0077 -p "RuntimeMaxSec=$((end-now+2))s" -p TimeoutStopSec=3s taskset -c "$one" /usr/bin/python3 -BE "$PWD/gates/overlap_v10_target_credits.py" "$d" "$end" >"$d/measure.log" 2>&1 || rc=$?;echo "metadata=$rc" >>"$d/exits.log";exit "$rc"
