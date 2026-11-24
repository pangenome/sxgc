#!/usr/bin/env bash
# The first actual native-wide dictionary sort: raw 3 GB run14e input. No u32
# overflow mocks. Do not run this until the raw-to-product 1 GB gate is banked.
set -euo pipefail
cd "$(dirname "$0")/.."; REPO=$PWD; root=/mnt/nvme2n1/erikg/TAPEREADER
case ${1:-disk-bitmap} in disk-bitmap) profile=packed-3g;; ram-bitmap) profile=packed-3g-bitmap;; *) echo 'usage: overlap_three_gate.sh [disk-bitmap|ram-bitmap]' >&2; exit 64;; esac
python3 - "$profile" <<'PY'
import json,pathlib,sys
p=pathlib.Path('gates/receipts/packed-raw-1g')
def read(name):return json.loads((p/name).read_text())
assert read('identity.json')['verified'] and read('identity.json')['sha256']=='b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3'
assert read('build.json')['packed_tape_retired'] and read('goals.json')['promotion_under_3x'] and read('goals.json')['handoff_io_pass']
if sys.argv[1]=='packed-3g-bitmap':
    p=p.parent/'packed-native-3g'
    assert read('identity.json')['verified'] and read('identity.json')['sha256']=='3935e8f1d220092d2cd12081938748e8a62f02e882bd021781ec7c3d0cabe7f3'
    assert read('build.json')['profile']=='packed-3g' and not read('goals.json')['normalized_io_pass']
PY
(cd xsa && cargo build --release -p pfp --bin pfp_sort_r --bin overlap_resident)
XSA_RESIDENT_WIDE_DICT=1 python3 gates/overlap_resident_test.py
D=$(mktemp -d "$root/sandbox/${profile}-XXXXXX"); T=$(mktemp -d /tmp/packed-native-3g-XXXXXX); echo "receipt=$D tools=$T"
cc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$T/monitor.so"
g++ -O2 -Wall -Wextra -Werror -std=c++17 gates/overlap_prefix_oracle.cpp -o "$T/oracle"
g++ -O2 -Wall -Wextra -Werror -std=c++17 gates/overlap_lexparse.cpp -o "$T/lexparse"
extra_hash=()
if [[ $profile == packed-3g-bitmap ]]; then
    cc -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC gates/finish_bitmap_tmpfs.c -o "$T/bitmap.so"
    python3 gates/finish_bitmap_test.py "$T/bitmap.so" |tee "$D/layout-test.log"
    extra_hash+=("$T/bitmap.so")
fi
cp xsa/target/release/{pfp_sort_r,overlap_resident} "$T/"; mkdir "$D/in"
cp --reflink=never /mnt/nvme2n1/erikg/KEYRADIX/c3g.txt "$D/in/corpus"; cp --reflink=never "$D/in/corpus" "$D/oracle-corpus"
cp --reflink=never "$root/bins-p1/run14/remap-256.bin" "$D/in/remap"
finish=$root/bins-p1/bin/xsa_finish
[[ $(stat -c%s "$D/in/corpus") == 3000000000 ]]
printf '%s  %s\n' 38dd65a7ce10960319824a43d6974ccc34d4f307bc48f31bbb97de0d26fdaca6 "$D/in/remap" 1a8db5181b0484c25a991fb1ca3dea6be66be70a4f52710843d23982ba8a001c "$finish" |sha256sum -c -
{ git rev-parse HEAD; git status --porcelain; sha256sum "$T/"{monitor.so,lexparse,oracle,overlap_resident,pfp_sort_r} "${extra_hash[@]}" "$finish" "$D/in/remap"; } >"$D/binaries.sha"
if [[ $profile == packed-3g-bitmap ]]; then
    PYTHONPATH=gates python3 - "$D" <<'PY'
from pathlib import Path
import sys
from overlap_three_score import binaries
base=binaries(Path('gates/receipts/packed-native-3g')); arm=binaries(Path(sys.argv[1]))
assert base.keys()=={'lexparse','monitor.so','oracle','overlap_resident','pfp_sort_r','xsa_finish','remap'}
assert arm.keys()==(base.keys()|{'bitmap.so'}) and all(arm[k]==v for k,v in base.items())
PY
fi
python3 gates/private_cold.py "$D/in/"* "$D/oracle-corpus"; echo 'mincore: 0 resident private input pages' |tee "$D/cold.log"
run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p Nice=10)
set +e
"${run[@]}" -p MemoryMax=60G -p CPUQuota=400% /usr/bin/python3 "$REPO/gates/overlap_resident_gate.py" build "$D" "$T" "$profile" | \
"${run[@]}" -p MemoryMax=4200M -p CPUQuota=100% /usr/bin/python3 "$REPO/gates/overlap_resident_gate.py" oracle "$D" "$T" "$profile"
rc=("${PIPESTATUS[@]}");set -e;printf 'build=%s oracle=%s\n' "${rc[@]}" |tee "$D/exits.log"; [[ ${rc[*]} == '0 0' ]]
python3 gates/overlap_resident_gate.py certify "$D" "$profile"; python3 gates/overlap_three_score.py "$D"; echo "certified receipt=$D"
