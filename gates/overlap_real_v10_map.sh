#!/bin/bash
# Preparation only: 100,000,503 B of source prefix, not another full 100 GB scan.
set -euo pipefail
cd "$(dirname "$0")/..";now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+600 && $1 <= now+1000 )) || exit 75
end=$1;source gates/overlap_pin.sh;overlap_pin_series gates/overlap_real_v10_map.sh "$end"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
/usr/bin/python3 -BE gates/overlap_timing_idle.py
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$s/real-v10-map100-*/source.json" >/dev/null || { echo 'use the authenticated source receipt, do not repeat';exit 75; }
umask 077;d=$(mktemp -d "$s/real-v10-map100-XXXXXX");echo "REAL_V10_INPUTS=$d"
trap 'rc=$?; echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
printf '%s\n' "$SXGC_PINNED_SOURCE" >"$d/source-pin";sha256sum gates/overlap_real_v10_map.{py,sh} >"$d/sources.sha";cpu=$(/usr/bin/python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');rc=0
systemd-run --user --wait --quiet --pipe --collect --expand-environment=no -p CPUQuota=100% -p MemoryMax=256M -p MemorySwapMax=0 -p LimitFSIZE=128M -p LimitCORE=0 -p Nice=10 -p IOAccounting=yes -p UMask=0077 -p RuntimeMaxSec=120s -p TimeoutStopSec=3s taskset -c "$cpu" /usr/bin/python3 -BE "$PWD/gates/overlap_real_v10_map.py" "$d" "$end" >"$d/source.log" 2>&1 || rc=$?
echo "source=$rc" >>"$d/exits.log";(( rc == 0 )) || exit "$rc";echo "REAL_V10_INPUTS_AUTHENTICATED=$d"
