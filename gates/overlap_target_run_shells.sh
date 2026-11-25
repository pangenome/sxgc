#!/bin/bash
# Generated texts + authenticated small repository receipts ONLY. Never open
# the original raw/index or inherit the previous archive service's grant.
set -euo pipefail;cd "$(dirname "$0")/..";now=$(date +%s)
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1>now+25 && $1<=now+90 )) || exit 75
end=$1;[[ $(git rev-parse HEAD) == "$(git rev-parse refs/remotes/origin/main)" && ! -e gates/receipts/target-run-shells/result.json ]] || exit 75;source gates/overlap_pin.sh;overlap_pin_series gates/overlap_target_run_shells.sh "$end"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
umask 077;d=$(mktemp -d /tmp/run-shell-proof-XXXXXX);echo "RUN_SHELL_PROOF=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
echo "$SXGC_PINNED_SOURCE" >"$d/source.log";echo 'e50d468e8b0adfb05733f5b87b3cff34829c4a8c1aea50c865aa8bdfe4bb150f  /usr/bin/python3' >"$d/interpreter.sha";sha256sum -c "$d/interpreter.sha"
one=$(python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');now=$(date +%s);((end>now+20)) || exit 75;rc=0
systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=100% -p MemoryMax=192M -p MemorySwapMax=0 -p IOAccounting=yes -p Nice=10 -p LimitCORE=0 -p LimitSTACK=8M -p LimitFSIZE=1M -p UMask=0077 -p "RuntimeMaxSec=$((end-now+1))s" -p TimeoutStopSec=2s taskset -c "$one" /usr/bin/python3 -BE "$PWD/gates/overlap_target_run_shells.py" "$d" "$end" >"$d/review.log" 2>&1 || rc=$?;echo "proof=$rc" >>"$d/exits.log";exit "$rc"
