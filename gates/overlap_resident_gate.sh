#!/usr/bin/env bash
# One cold, RAM-rich comparison on the exact 1 GB rung. The prepare/copy,
# raw oracle (including its private text), and post-run hash are not producer IO.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
case ${1:-narrow} in narrow|wide|wide-handoff|packed-raw|packed-radix-1|packed-radix-2|packed-radix-4) profile=${1:-narrow};; *) echo 'usage: overlap_resident_gate.sh [narrow|wide|wide-handoff|packed-raw|packed-radix-{1,2,4}]' >&2; exit 2;; esac
(cd xsa && cargo build --release -p pfp --bin pfp_sort_r --bin overlap_resident)
if [[ $profile != narrow ]]; then XSA_RESIDENT_WIDE_DICT=1 python3 gates/overlap_resident_test.py; else XSA_RESIDENT_WIDE_DICT=0 python3 gates/overlap_resident_test.py; fi
ROOT=/mnt/nvme2n1/erikg/TAPEREADER; S=$ROOT/sandbox; R=$ROOT/climb/rung1
D=$(mktemp -d "$S/resident-rung1-$profile-XXXXXX"); T=$(mktemp -d /tmp/resident-bin-XXXXXX); echo "receipt=$D tools=$T"
g++ -O2 -Wall -Wextra -Werror -std=c++17 gates/overlap_prefix_oracle.cpp -o "$T/oracle"
cc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$T/monitor.so"
mkdir "$D/in"
cp --reflink=never "$R/pile-slice1g.bin" "$D/in/corpus"
cp --reflink=never "$R/remap-256.bin" "$D/in/remap"
if [[ $profile == packed-* ]]; then
    g++ -O2 -Wall -Wextra -Werror -std=c++17 gates/overlap_lexparse.cpp -o "$T/lexparse"
else
    cp --reflink=never "$S/dict-adj-kIPyZL/columns/"{parse.u32,dictionary.refs} "$D/in/"
fi
cp --reflink=never "$S/parse-ranks-Ys27jk/work/inverse.u32" "$D/inverse.check"
cp --reflink=never "$R/pile-slice1g.bin" "$D/oracle-corpus"
cp xsa/target/release/{overlap_resident,pfp_sort_r} "$T/"; extra=()
if [[ $profile == packed-* ]]; then extra+=("$T/lexparse"); fi
if [[ $profile == packed-radix-* ]]; then
    cc -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC gates/finish_bitmap_tmpfs.c -o "$T/bitmap.so";extra+=("$T/bitmap.so")
    python3 gates/finish_bitmap_test.py "$T/bitmap.so" |tee "$D/layout-test.log";python3 gates/overlap_radix_test.py "$T/overlap_resident" |tee "$D/seam-fixtures.log"
fi
{ git rev-parse HEAD; git status --porcelain; sha256sum "$T/"{monitor.so,oracle,overlap_resident,pfp_sort_r} /tmp/xsa_finish_cur "$D/in/remap" "${extra[@]}"; } > "$D/binaries.sha"
python3 gates/private_cold.py "$D/in/"* "$D/inverse.check" "$D/oracle-corpus"
echo 'all private stage inputs, rank check and oracle copy: mincore zero pages' | tee "$D/cold.log"
# A fresh user service for each meter, no cache flush, swap or machine-wide dials.
run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p Nice=10)
set +e
"${run[@]}" -p MemoryMax=20G -p CPUQuota=400% /usr/bin/python3 "$REPO/gates/overlap_resident_gate.py" build "$D" "$T" "$profile" | \
"${run[@]}" -p MemoryMax=1400M -p CPUQuota=100% /usr/bin/python3 "$REPO/gates/overlap_resident_gate.py" oracle "$D" "$T"
rc=("${PIPESTATUS[@]}");set -e;printf 'build=%s oracle=%s\n' "${rc[@]}" |tee "$D/exits.log"
[[ ${rc[*]} == '0 0' ]]
python3 gates/overlap_resident_gate.py certify "$D"
if [[ $profile == packed-radix-* ]];then python3 gates/overlap_resident_report.py "$D" >"$D/summary.md";elif [[ $profile != narrow ]]; then python3 gates/overlap_slowdown_gate.py "$D"; fi
echo "certified receipt=$D"
