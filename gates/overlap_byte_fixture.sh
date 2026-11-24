#!/usr/bin/env bash
# Only 64 small windows of a pinned public-domain book and two generated finite dictionaries.
set -euo pipefail
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1<=1795531169 && $1>$(date +%s)+120 )) || exit 75
cd "$(dirname "$0")/..";source gates/overlap_pin.sh;overlap_pin_series gates/overlap_byte_fixture.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$s/byte-local-fixture-*" >/dev/null || exit 75
d=$(mktemp -d "$s/byte-local-fixture-XXXXXX");echo "BYTE_FIXTURE receipt=$d";end=$1;p=$PWD/gates/overlap_byte_fixture.py
/usr/bin/python3 -BE "$p" admit "$d" "$end"
comp(){ local n=$((end-$(date +%s)));((n>0)) || return 75;timeout -k 2 "$n" "$@"; }
{
 rustc --version;git rev-parse HEAD
 comp rustc -Dwarnings --edition=2021 -O --test xsa/pfp/tests/overlap_compact_order.rs -o "$d/tiny-test"
 comp "$d/tiny-test" --test-threads=1
 comp rustc -Dwarnings --edition=2021 -O gates/overlap_byte_fixture.rs -o "$d/fixture"
 sha256sum "$d/fixture" >"$d/fixture.sha"
} >"$d/build-and-fixtures.log" 2>&1
set +e;systemd-run --user --quiet --wait --pipe --collect -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 /usr/bin/python3 -BE "$p" measure "$d" "$end"
rc=$?;set -e;echo "fixture=$rc" >"$d/exits.log";((rc==0)) || exit "$rc"
cmp <(/usr/bin/python3 -BE "$p" score "$d/fixture.log") "$d/score.json"
echo "BYTE_FIXTURE_CHECKED receipt=$d"
