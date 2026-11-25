#!/bin/bash
# Generated selector qualification only. No corpus argument or published product.
set -euo pipefail;cd "$(dirname "$0")/..";umask 077
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75;python3 -BE gates/overlap_timing_idle.py
d=$(mktemp -d /tmp/prefix-window-tools-XXXXXX);echo "WINDOW_TOOLS=$d";trap 'rc=$?;echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
mkdir "$d/src";for f in overlap_prefix_real_windows.cpp overlap_prefix_window_select.hpp overlap_raw_prefix_intervals.hpp;do cp "gates/$f" "$d/src/$f";cmp "gates/$f" "$d/src/$f";done;(cd "$d/src";sha256sum ./*) >"$d/sources.sha";git rev-parse HEAD >"$d/base.log";echo '1353e9bdd29a7295c7226bf6c63abccce056d8cac31f112e5cdbecc3f28c2769  /usr/bin/g++' >"$d/compiler.sha";sha256sum -c "$d/compiler.sha"
one=$(python3 -BE -c 'import os;print(min(os.sched_getaffinity(0)))');end=$(( $(date +%s)+90 ));cd "$d"
u=(systemd-run --user --quiet --pipe --wait --collect --expand-environment=no -p CPUQuota=100% -p MemorySwapMax=0 -p Nice=10 -p IOAccounting=yes -p LimitSTACK=8M -p LimitCORE=0 -p LimitFSIZE=8M -p TimeoutStopSec=3s -p UMask=0077 -p "WorkingDirectory=$d")
phase(){ local name=$1 rc=0;shift;"$@" >"$d/$name.log" 2>&1 || rc=$?;echo "$name=$rc" >>"$d/exits.log";return "$rc"; }
phase compile "${u[@]}" -p MemoryMax=384M -p RuntimeMaxSec=40s taskset -c "$one" /usr/bin/g++ -std=c++20 -O3 -Wall -Wextra -Werror "$d/src/overlap_prefix_real_windows.cpp" -o "$d/probe"
(cd "$d/src";sha256sum -c "$d/sources.sha");sha256sum "$d/probe" >"$d/tool.sha";phase fixture "${u[@]}" -p MemoryMax=256M -p RuntimeMaxSec=24s taskset -c "$one" "$d/probe" fixture "$end"
