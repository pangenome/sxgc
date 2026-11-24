#!/usr/bin/env bash
set -euo pipefail
[[ ( $# == 2 && $1 == 1g || $# == 4 && $1 == 10g && $4 =~ ^[a-f0-9]{64}$ ) && $2 =~ ^[1-9][0-9]{9}$ ]] || { echo 'usage: overlap_segmented_gate.sh 1g DEADLINE | 10g DEADLINE REVIEWED_1G_DIR REVIEWED_RECEIPTS_SHA256' >&2; exit 64; }
remaining(){ local s=$(($2-$(date +%s))); ((s>0)) || return 75; echo "$s"; }
cd "$(dirname "$0")/.."; repo=$PWD
reserve=240;[[ $1 != 10g ]] || reserve=1800
[[ $(remaining "$@") -ge $reserve ]] || { echo 'insufficient remaining time for front + independent decode' >&2;exit 75; }
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock"; flock -n 9 || exit 75
/usr/bin/python3 -E gates/overlap_timing_idle.py
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox; d=$(mktemp -d "$s/segmented-$1-XXXXXX"); echo "intermediate receipt=$d"
n=$(remaining "$@"); timeout -k 1 "$n" g++ -O2 -std=c++17 -Wall -Wextra -Werror -DSEGMENTED_INTERMEDIATE gates/overlap_lexparse100.cpp -o "$d/front"
n=$(remaining "$@"); timeout -k 1 "$n" g++ -O2 -std=c++17 -Wall -Wextra -Werror gates/overlap_segmented_verify.cpp -lcrypto -o "$d/verify"
{ git rev-parse HEAD; sha256sum "$d/front" "$d/verify" gates/overlap_{lexparse100.cpp,segmented_gate.py,segmented_decode.py,segmented_gate.sh,segmented_verify.cpp}; } >"$d/binaries.log"
service(){ local mem=$1; shift; systemd-run --user --quiet --collect --wait --pipe -p MemoryMax="$mem" -p MemorySwapMax=0 -p CPUQuota=100% -p IOAccounting=yes -p Nice=10 /usr/bin/python3 -E "$@"; }
n=$(remaining "$@"); service 192M "$repo/gates/overlap_segmented_gate.py" "$1" "$d" "$d/front" "$2" "${@:3}"
n=$(remaining "$@"); mem=3G; [[ $1 != 10g ]] || mem=16G
service "$mem" "$repo/gates/overlap_segmented_decode.py" "$1" "$d" "$2"
