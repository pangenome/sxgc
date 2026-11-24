#!/bin/bash
# Two bounded helper invocations; neither dispatches a corpus row producer.
set -euo pipefail
cd "$(dirname "$0")/.."
d=${2:?private output}; [[ $d == /tmp/verified-row-fence-* && -d $d && ! -L $d && -O $d && $(stat -c %a "$d") == 700 ]]
cg=/sys/fs/cgroup$(awk -F'::' '$1=="0" {print $2}' /proc/self/cgroup)
[[ $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '100000 100000' ]]
case ${1:?phase} in
  fixtures)
    [[ $(<"$cg/memory.max") == 536870912 ]]
    for bin in real fixture blocks; do [[ ! -e $d/$bin && ! -L $d/$bin ]]; done
    # These words belong to bash in the service, not systemd-run's ExecStart expansion.
    flags=(-std=c++20 -O2 -g -fsanitize=undefined -fno-sanitize-recover=all -Wall -Wextra -Werror)
    /usr/bin/g++ "${flags[@]}" gates/overlap_finish_row_fence.cpp -lcrypto -o "$d/real"
    /usr/bin/g++ "${flags[@]}" -DROW_FENCE_FIXTURE gates/overlap_finish_row_fence.cpp -lcrypto -o "$d/fixture"
    /usr/bin/g++ "${flags[@]}" gates/overlap_finish_row_fence_blocks.cpp -o "$d/blocks"
    /usr/bin/python3 -BE gates/overlap_finish_row_fence_test.py fixtures "$d/fixture" "$d/real" "$d/blocks"
    /usr/bin/python3 -BE gates/overlap_finish_credits_test.py;;
  member) [[ $(<"$cg/memory.max") == 201326592 ]]; /usr/bin/python3 -BE gates/overlap_finish_row_fence_test.py frozen "$d/blocks";;
  *) exit 75;;
esac
