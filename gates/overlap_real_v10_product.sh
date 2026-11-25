#!/bin/bash
# Two sequential fixed 100 MB arms. A verified reference or closed row proof is NEVER retried here.
set -euo pipefail
cd "$(dirname "$0")/..";now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+900 && $1 <= now+1500 )) || exit 75
end=$1;source gates/overlap_pin.sh;overlap_pin_series gates/overlap_real_v10_product.sh "$end"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
python3 -BE gates/overlap_timing_idle.py;s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
for q in "$s"/real-v10-pair100-*;do [[ ! -e $q ]] || { echo "HOLD: inspect prior real product attempt first: $q";exit 75; };done
umask 077;d=$(mktemp -d "$s/real-v10-pair100-XXXXXX");echo "REAL_V10_PAIR=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
read -r cores checker < <(python3 -BE gates/overlap_real_v10_score.py admit "$d" "$end")
phase(){ local tag=$1 rc=0;shift; "$@" >"$d/$tag.log" 2>&1 || rc=$?;echo "$tag=$rc" >>"$d/exits.log";return "$rc"; }
u=(systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=400% -p MemoryMax=2G -p MemorySwapMax=0 -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 -p LimitSTACK=8M -p LimitFSIZE=192M -p UMask=0077 -p TimeoutStopSec=5s)
for arm in reference candidate;do
 python3 -BE gates/overlap_timing_idle.py;now=$(date +%s);if [[ $arm == reference ]];then until=$(( now+665<end-650 ? now+665 : end-650 ));else until=$(( now+720<end-25 ? now+720 : end-25 ));fi
 (( until>now+560 )) || { echo 'HOLD: not enough time for a separate complete product arm';exit 75; };echo "START_REAL_V10=$arm until=$until"
 phase "arm-$arm" "${u[@]}" -p "RuntimeMaxSec=$((until-now+10))s" taskset -c "$cores" /usr/bin/python3 -BE "$PWD/gates/overlap_real_v10_product.py" "$arm" "$d" "$until" "$checker"
 echo "CERTIFIED_REAL_V10=$arm"
done
phase score python3 -BE gates/overlap_real_v10_score.py score "$d";echo "COMPLETE_REAL_V10_PAIR=$d"
