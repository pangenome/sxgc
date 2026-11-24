#!/usr/bin/env bash
# Real text offsets exceed u32. New raw→packed→wide producer→same v8 finish.
# No construction state from the standalone census is loaded by any worker.
set -euo pipefail
cd "$(dirname "$0")/.."; repo=$PWD; root=/mnt/nvme2n1/erikg; ref=$root/lcpruns-gate/g3/scratch
pin=8513cccb7d0ade1f4917752e3e8be33c3f7e49e3
profile=packed-10g-bitmap; extra=(); matched=0
pinned_repo() {
    [[ $(git -C "$1" rev-parse HEAD) == "$pin" && -z $(git -C "$1" status --porcelain) ]] || { echo 'need clean pinned build repo' >&2;exit 64; }
}
# Detect old (non-locking) harnesses and timing workers as well as locking
# concurrent matched invocations. The operator still schedules the host.
idle() {
    python3 - <<'PYIDLE'
from pathlib import Path
import os
for cmd in Path('/proc').glob('[0-9]*/cmdline'):
    if int(cmd.parent.name) in (os.getpid(),os.getppid()):continue
    try:a=[Path(os.fsdecode(x)).name for x in cmd.read_bytes().split(b'\0') if x]
    except (OSError,ValueError):continue
    if any(x in ('overlap_resident','pfp_sort_r','xsa_finish_cur','overlap_resident_gate.py','overlap_ten_product.sh') or x.startswith('overlap_') and x.endswith('.sh') for x in a):raise SystemExit('other timing arm: '+str(cmd.parent))
PYIDLE
}
clock_check() { if [[ $(date +%s) -ge $deadline ]];then echo 'deadline expired' >&2;exit 75;fi; }
case $#:${1:-} in
0:) ;;
3:--prepare-tools)
    unset PYTHONOPTIMIZE;src=$(realpath "$2"); t=$(realpath -m "$3");pinned_repo "$src";mkdir "$t";cd "$src"
    export XSA_BUILD_DICT_OPENMP=1 CC=gcc CARGO_TARGET_DIR="$t/target"
    (cd xsa && cargo test -j1 --locked --release -p pfp --bin overlap_resident --bin pfp_sort_r && cargo build -j1 --locked --release -p pfp --bin overlap_resident --bin pfp_sort_r)
    cp "$t/target/release/"{pfp_sort_r,overlap_resident} "$t/"
    cc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$t/monitor.so"
    cc -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC gates/finish_bitmap_tmpfs.c -o "$t/bitmap.so"
    g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_lexparse10.cpp -o "$t/lexparse"
    g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_row43_oracle.cpp -o "$t/oracle"
    pinned_repo "$src";printf '%s\n%s\n' "$pin" "$src" >"$t/source"
    (cd "$t" && sha256sum monitor.so bitmap.so lexparse oracle overlap_resident pfp_sort_r source > manifest.sha);exit 0;;
5:--profile)
    unset PYTHONOPTIMIZE;profile=$2;src=$(realpath "$3");t=$(realpath "$4");deadline=$5
    [[ $profile =~ ^packed-10g-l[124]-b[01]-t[124]-r[01]$ && $deadline =~ ^[1-9][0-9]{0,10}$ ]] || exit 64
    clock_check;pinned_repo "$src";[[ $(cat "$t/source") == "$(printf '%s\n%s' "$pin" "$src")" ]];(cd "$t" && sha256sum -c manifest.sha)
    [[ -z $(git status --porcelain) ]] || { echo 'need clean harness' >&2;exit 64; }
    readelf -d "$t/overlap_resident" | grep 'NEEDED.*libgomp'
    PYTHONPATH=gates python3 - "$t" <<'PYBIN'
from pathlib import Path
import sys
from overlap_ten_score import FIXED
from overlap_resident_gate import sha
t=Path(sys.argv[1]);assert all(sha(t/k)==FIXED[k] for k in ('lexparse','oracle','monitor.so','bitmap.so'))
PYBIN
    exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9;idle;matched=1;extra=("$deadline");;
*) echo 'usage: overlap_ten_product.sh [--prepare-tools PINNED_REPO NEW_DIR | --profile packed-10g-l{1,2,4}-b{0,1}-t{1,2,4}-r{0,1} PINNED_REPO TOOLS DEADLINE_UNIX]' >&2;exit 64;;
esac
PYTHONPATH=gates python3 - <<'PY'
import json,shutil
from pathlib import Path
from overlap_resident_gate import TEN,ROOT
p=Path('gates/receipts');get=lambda s:json.loads((p/s).read_text()); assert get('packed-3g-bitmap/goals.json')['matched_bitmap_pass']
f=get('packed-front-10g/front-end.json');v=f['census'];assert f['rc']==0 and v['n']==TEN.n and 0<v['starts_above_u32']<v['m'] and v['D']>2**32 and v['alphabet']<=254
assert max(f['proposed_array_bytes'].values())<=TEN.arrays_gib*2**30 and f['parse_sort_extent']<2**31 and v['peak_counted']<=2*TEN.n
assert shutil.disk_usage(ROOT).free>=64_000_000_000 and TEN.anchor.is_file()
# Headroom is a refusal check, not permission to use the machine's free RAM.
mem=int(next(l.split()[1] for l in Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')))*1024
assert mem>=(TEN.shared_gib+32)*2**30 and shutil.disk_usage('/dev/shm').free>2**31
PY
d=$(mktemp -d "$root/TAPEREADER/sandbox/$profile-XXXXXX")
if ((matched));then
    cp "$t/source" "$d/build-source.log";echo "receipt=$d tools=$t"
    dict=${profile%-r*};XSA_RESIDENT_DICT_THREADS=${dict##*-t} OMP_WAIT_POLICY=PASSIVE GOMP_SPINCOUNT=0 python3 gates/overlap_radix_test.py "$t/overlap_resident" >"$d/seam-fixtures.log"
else
(cd xsa && cargo test --release -p pfp --bin overlap_resident --bin pfp_sort_r && cargo build --release -p pfp --bin overlap_resident --bin pfp_sort_r)
t=$(mktemp -d /tmp/overlap-product-ten-XXXXXX); echo "receipt=$d tools=$t"
cc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$t/monitor.so"
cc -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC gates/finish_bitmap_tmpfs.c -o "$t/bitmap.so"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_lexparse10.cpp -o "$t/lexparse"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_row43_oracle.cpp -o "$t/oracle"
cp xsa/target/release/{pfp_sort_r,overlap_resident} "$t/"
fi
python3 gates/finish_bitmap_test.py "$t/bitmap.so" |tee "$d/fixtures.log"
python3 gates/overlap_ten_front.py test "$t/lexparse" |tee -a "$d/fixtures.log"
python3 gates/overlap_row43_test.py "$t/oracle" "$t/overlap_resident" |tee -a "$d/fixtures.log"
if ((matched));then clock_check;fi
mkdir "$d/in"; [[ $(stat -c%s "$root/pf-10gb.txt") == 10000000000 ]];cp --reflink=never "$root/pf-10gb.txt" "$d/in/corpus"; cp --reflink=never "$d/in/corpus" "$d/oracle-corpus";cp --reflink=never "$ref/remap-256.bin" "$d/in/remap"
printf '%s  %s\n' 0ce35ac950b471c0da0e9a7773a6d82871d53d5de5a496bb9ffda6078b896780 "$d/in/remap" 9a0bde6739ba1819f8d298beca2c8a9bf53480fab19f637b49cf6e3c8e200fa7 /tmp/xsa_finish_cur |sha256sum -c -
{ git rev-parse HEAD; git status --porcelain; sha256sum "$t/"{monitor.so,bitmap.so,lexparse,oracle,overlap_resident,pfp_sort_r} /tmp/xsa_finish_cur "$d/in/remap"; } >"$d/binaries.sha"
python3 gates/private_cold.py "$d/in/"* "$d/oracle-corpus"; echo 'cold: private corpus, independent oracle text and remap checked zero resident pages' |tee "$d/cold.log"
if ((matched));then idle;clock_check;fi
run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p Nice=10 -p LimitNOFILE=8192)
set +e
"${run[@]}" -p MemoryMax=200G -p CPUQuota=400% /usr/bin/python3 "$repo/gates/overlap_resident_gate.py" build "$d" "$t" "$profile" "${extra[@]}" | \
"${run[@]}" -p MemoryMax=14000M -p CPUQuota=100% /usr/bin/python3 "$repo/gates/overlap_resident_gate.py" oracle "$d" "$t" "$profile" "${extra[@]}"
rc=("${PIPESTATUS[@]}");set -e;printf 'build=%s oracle=%s\n' "${rc[@]}" |tee "$d/exits.log"; [[ ${rc[*]} == '0 0' ]]
python3 gates/overlap_resident_gate.py certify "$d" "$profile"; python3 gates/overlap_ten_score.py "$d"; if ((!matched));then python3 gates/overlap_resident_report.py "$d" >"$d/summary.md";fi;echo "certified receipt=$d"
