#!/usr/bin/env bash
# Explicit 10 GB census only. Never chains or admits a larger input.
set -euo pipefail
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 <= 1795531169 && $1-$(date +%s)>1500 )) || { echo 'need an admissible deadline' >&2;exit 75; }
cd "$(dirname "$0")/..";source gates/overlap_pin.sh;overlap_pin_series gates/overlap_vocab_gate.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;d=$(mktemp -d "$s/vocab-10g-XXXXXX");echo "VOCAB10 receipt=$d"
py=(/usr/bin/python3 -BE "$PWD/gates/overlap_vocab_gate.py");"${py[@]}" preflight "$d" "$1"
remain(){ local n=$(( $1-$(date +%s) ));((n>0)) || return 75;echo "$n"; }
flags=(-O2 -Wall -Wextra -Werror -std=c++17);n=$(remain "$1")
{
 timeout "$n" g++ "${flags[@]}" gates/overlap_vocab_census.cpp -o "$d/census"
 timeout "$(remain "$1")" g++ "${flags[@]}" gates/overlap_vocab_reference.cpp -lcrypto -o "$d/reference"
 timeout "$(remain "$1")" g++ "${flags[@]}" -DVOCAB_REFERENCE_TINY gates/overlap_vocab_reference.cpp -lcrypto -o "$d/tiny-reference"
 timeout "$(remain "$1")" /usr/bin/python3 -BE gates/overlap_vocab_fixture_test.py "$d/census"
 timeout "$(remain "$1")" /usr/bin/python3 -BE gates/overlap_vocab_reference_test.py "$d/tiny-reference" "$d/reference"
 timeout "$(remain "$1")" /usr/bin/python3 -BE gates/overlap_vocab_plan_test.py
} >"$d/fixtures.log" 2>&1
{ git rev-parse HEAD;sha256sum "$d/"{census,reference};g++ --version; } >"$d/binaries.log"
run=(systemd-run --user --collect --quiet --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p IOAccounting=yes -p LimitNOFILE=1024 -p LimitCORE=0 -p Nice=10)
for role in census reference;do
 rc=0;"${run[@]}" "${py[@]}" "$role" "$d" "$1" || rc=$?;echo "$role=$rc" >>"$d/exits.log";((rc==0)) || exit "$rc"
done
echo "VOCAB10_VERIFIED receipt=$d"
