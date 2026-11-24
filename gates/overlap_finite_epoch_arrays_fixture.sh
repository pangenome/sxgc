#!/bin/bash
# Generated/plan only; no corpus service. Caller holds timing lock.
set -euo pipefail; cd "$(dirname "$0")/.."; d=$1
[[ $# == 1 && $d == /tmp/epoch-array-fence-* && -d $d && $d == "$(realpath -- "$d")" && -O $d && $(stat -c %a "$d") == 700 ]]
cg=/sys/fs/cgroup$(awk -F'::' '$1=="0" {print $2}' /proc/self/cgroup)
[[ $(<"$cg/memory.max") == 805306368 && $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '200000 100000' ]]
for f in emit tmp model.json;do [[ ! -e $d/$f && ! -L $d/$f ]];done
umask 077;mkdir "$d/tmp";export TMPDIR=$d/tmp OMP_DYNAMIC=FALSE OMP_STACKSIZE=1M OMP_THREAD_LIMIT=4
g++ -std=c++20 -O2 -g0 -Wall -Wextra -Werror -fopenmp -fsanitize=undefined -fno-sanitize-recover=all xsa/pfp/tools/finite_emit64.cpp -lcrypto -o "$d/emit"
/usr/bin/python3 -BE gates/overlap_finite_epoch_arrays.py "$d/emit" >"$d/model.json"
/usr/bin/python3 -BE gates/overlap_finite_epoch_arrays_test.py "$d/emit"
/usr/bin/python3 -BE gates/overlap_finite_epoch64_study.py score gates/receipts/finite-epoch64-100m | cmp - gates/receipts/finite-epoch64-100m/score.json
/usr/bin/python3 -BE gates/overlap_finish_credits.py review | cmp - gates/receipts/finish-credits-100m/review.json
sha256sum "$d/emit" "$d/model.json"
