#!/bin/bash
# Generated source-reader handoff and archive-only reduction, not a product service.
set -euo pipefail;cd "$(dirname "$0")/..";d=$2;[[ $# == 2 && $d == /tmp/v8-fold-geometry-* && $d == "$(realpath -- "$d")" && -O $d && $(stat -c %a "$d") == 700 ]]
cg=/sys/fs/cgroup$(awk -F'::' '$1=="0" {print $2}' /proc/self/cgroup)
[[ $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '100000 100000' ]];umask 077
case $1 in
 tools)
    [[ $(<"$cg/memory.max") == 536870912 && $(g++ -dumpfullversion) == 13.3.0 ]];for f in pinned-lcp-column.inc probe relay-fixture relay-real blocks model.json;do [[ ! -e $d/$f && ! -L $d/$f ]];done
    flags=(-std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all)
    python3 -BE gates/overlap_finish_fold_fixture.py class "$d";g++ "${flags[@]}" -I"$d" gates/overlap_finish_fold_probe.cpp -o "$d/probe";"$d/probe" unit
    g++ "${flags[@]}" gates/overlap_finish_row_fence.cpp -lcrypto -o "$d/relay-real";g++ "${flags[@]}" -DROW_FENCE_FIXTURE gates/overlap_finish_row_fence.cpp -lcrypto -o "$d/relay-fixture";g++ "${flags[@]}" gates/overlap_finish_row_fence_blocks.cpp -o "$d/blocks"
    python3 -BE gates/overlap_finish_fold_fixture.py model >"$d/model.json";python3 -BE gates/overlap_finish_fold_fixture.py relay "$d";python3 -BE gates/overlap_finish_row_fence_test.py fixtures "$d/relay-fixture" "$d/relay-real" "$d/blocks"
    sha256sum "$d"/{pinned-lcp-column.inc,probe,relay-fixture,relay-real,blocks,model.json} >"$d/tools.sha";;
 handoff)
    [[ $(<"$cg/memory.max") == 134217728 && ! -e $d/arm && ! -e $d/stop.requested ]];sha256sum -c "$d/tools.sha"
    python3 -BE gates/overlap_finish_fold_fixture.py partial "$d";;
 *) exit 75;;
esac
