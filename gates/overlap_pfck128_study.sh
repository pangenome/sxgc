#!/bin/bash
# One cold frozen-readiness probe only. Never start the row producer, sink or sweep.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${1:-} == --fixture-tools && $# == 2 ]]; then
    d=$2; [[ -d $d && $d == "$(realpath -- "$d")" && -O $d && $(stat -c %a "$d") == 700 ]]
    cg=/sys/fs/cgroup$(awk -F'::' '$1 == "0" {print $2}' /proc/self/cgroup)
    [[ $(<"$cg/memory.max") == 536870912 && $(<"$cg/memory.swap.max") == 0 && $(<"$cg/cpu.max") == '100000 100000' ]]
    for f in proof monitor.so ready-run;do [[ ! -e $d/$f && ! -L $d/$f ]];done
    /usr/bin/g++ -std=c++20 -O2 -Wall -Wextra -Werror -fsanitize=undefined -fno-sanitize-recover=all gates/overlap_pfck128_proof.cpp -o "$d/proof"
    /usr/bin/cc -std=gnu11 -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$d/monitor.so"
    install -m 500 gates/overlap_pfck128_worker.sh "$d/ready-run"
    /usr/bin/python3 -BE gates/overlap_pfck128_test.py "$d/proof"; exit 0
fi
# Keep the failed 1 MiB main-stack profile closed even on a fresh host. The
# fixture/tools arm above has no corpus or historical executable entry.
if [[ -e gates/receipts/pfck128-ready-entry-failure/source.json ]]; then
    echo 'HOLD: frozen finish main frame exceeds this readiness stack grant' >&2; exit 75
fi
now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+600 && $1 <= now+1000 )) || exit 75
end=$1; [[ ! -e gates/receipts/pfck128-ready-100m && -f gates/receipts/finish-credits-100m/review.json ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_pfck128_study.sh "$end"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$s/pfck128-ready-100m-*" >/dev/null || exit 75
umask 077;d=$(mktemp -d "$s/pfck128-ready-100m-XXXXXX");echo "PFCK128_STUDY receipt=$d";p=$PWD/gates/overlap_pfck128_study.py
trap 'rc=$?; echo launcher=$rc >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
/usr/bin/python3 -BE "$p" admit "$d" "$end"
u=(systemd-run --user --quiet --wait --collect --pipe --expand-environment=no -p MemorySwapMax=0 -p LimitCORE=0 -p Nice=10 -p IOAccounting=yes)
"${u[@]}" -p CPUQuota=100% -p MemoryMax=512M -p RuntimeMaxSec=90 /usr/bin/env -u BASH_ENV -u ENV /bin/bash --noprofile --norc "$PWD/gates/overlap_pfck128_study.sh" --fixture-tools "$d" >"$d/fixtures.log" 2>&1
for phase in setup ready cold proof;do
    props=(-p MemoryMax=192M -p CPUQuota=100% -p RuntimeMaxSec=65)
    if [[ $phase == ready ]];then props=(-p MemoryMax=192M -p CPUQuota=200% -p RuntimeMaxSec=65 -p LimitDATA=64M -p LimitSTACK=1M -p LimitFSIZE=6250072);fi
    if [[ $phase == proof ]];then props=(-p MemoryMax=128M -p CPUQuota=100% -p RuntimeMaxSec=65);fi
    set +e;"${u[@]}" "${props[@]}" /usr/bin/python3 -BE "$p" "$phase" "$d" "$end";rc=$?;set -e;echo "$phase=$rc" >>"$d/exits.log";((rc==0)) || exit "$rc"
done
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "PFCK128_STUDY_COMPLETE receipt=$d"
