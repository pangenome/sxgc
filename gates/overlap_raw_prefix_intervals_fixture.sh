#!/bin/bash
# Small exact-equality mechanism only. Snapshot these two source files before compiling.
set -euo pipefail;cd "$(dirname "$0")/..";umask 077
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
d=$(mktemp -d /tmp/raw-prefix-proof-XXXXXX);echo "PREFIX_FIXTURE=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
mkdir -m 700 "$d/src";cp gates/overlap_raw_prefix_intervals{.hpp,_test.cpp} "$d/src/";for f in "$d/src"/*;do cmp "$f" "gates/$(basename "$f")";done;(cd "$d/src";sha256sum ./*)>"$d/sources.sha";git rev-parse HEAD >"$d/checkout-base.log";sha256sum /usr/bin/g++ >"$d/compiler.sha";one=$(python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');end=$(( $(date +%s)+100 ));cd "$d"
u=(systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=100% -p MemorySwapMax=0 -p Nice=10 -p IOAccounting=yes -p LimitSTACK=8M -p LimitCORE=0 -p LimitFSIZE=16M -p TimeoutStopSec=3s -p UMask=0077 -p "WorkingDirectory=$d")
phase(){ local name=$1 rc=0;shift;"$@" >"$d/$name.log" 2>&1 || rc=$?;echo "$name=$rc" >>"$d/exits.log";return "$rc"; }
phase compile "${u[@]}" -p MemoryMax=384M -p RuntimeMaxSec=40s taskset -c "$one" /usr/bin/g++ -O3 -std=c++20 -Wall -Wextra -Werror "$d/src/overlap_raw_prefix_intervals_test.cpp" -o "$d/probe"
(cd "$d/src";sha256sum -c "$d/sources.sha");sha256sum "$d/probe" >"$d/tool.sha";phase generated "${u[@]}" -p MemoryMax=128M -p RuntimeMaxSec=45s taskset -c "$one" "$d/probe" "$end"
