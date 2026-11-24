#!/usr/bin/env bash
# Real text offsets exceed u32. New raw→packed→wide producer→same v8 finish.
# No construction state from the standalone census is loaded by any worker.
set -euo pipefail
cd "$(dirname "$0")/.."; repo=$PWD; root=/mnt/nvme2n1/erikg; ref=$root/lcpruns-gate/g3/scratch
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
(cd xsa && cargo test --release -p pfp --bin overlap_resident --bin pfp_sort_r && cargo build --release -p pfp --bin overlap_resident --bin pfp_sort_r)
d=$(mktemp -d "$root/TAPEREADER/sandbox/packed-10g-bitmap-XXXXXX"); t=$(mktemp -d /tmp/overlap-product-ten-XXXXXX); echo "receipt=$d tools=$t"
cc -O2 -Wall -Wextra -Werror -shared -fPIC -pthread gates/finish_stop_monitor.c -o "$t/monitor.so"
cc -O2 -std=c11 -Wall -Wextra -Werror -shared -fPIC gates/finish_bitmap_tmpfs.c -o "$t/bitmap.so"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_lexparse10.cpp -o "$t/lexparse"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_row43_oracle.cpp -o "$t/oracle"
cp xsa/target/release/{pfp_sort_r,overlap_resident} "$t/"
python3 gates/finish_bitmap_test.py "$t/bitmap.so" |tee "$d/fixtures.log"
python3 gates/overlap_ten_front.py test "$t/lexparse" |tee -a "$d/fixtures.log"
python3 gates/overlap_row43_test.py "$t/oracle" "$t/overlap_resident" |tee -a "$d/fixtures.log"
mkdir "$d/in"; [[ $(stat -c%s "$root/pf-10gb.txt") == 10000000000 ]];cp --reflink=never "$root/pf-10gb.txt" "$d/in/corpus"; cp --reflink=never "$d/in/corpus" "$d/oracle-corpus";cp --reflink=never "$ref/remap-256.bin" "$d/in/remap"
printf '%s  %s\n' 0ce35ac950b471c0da0e9a7773a6d82871d53d5de5a496bb9ffda6078b896780 "$d/in/remap" 9a0bde6739ba1819f8d298beca2c8a9bf53480fab19f637b49cf6e3c8e200fa7 /tmp/xsa_finish_cur |sha256sum -c -
{ git rev-parse HEAD; git status --porcelain; sha256sum "$t/"{monitor.so,bitmap.so,lexparse,oracle,overlap_resident,pfp_sort_r} /tmp/xsa_finish_cur "$d/in/remap"; } >"$d/binaries.sha"
python3 gates/private_cold.py "$d/in/"* "$d/oracle-corpus"; echo 'cold: private corpus, independent oracle text and remap checked zero resident pages' |tee "$d/cold.log"
profile=packed-10g-bitmap; run=(systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p Nice=10 -p LimitNOFILE=8192)
set +e
"${run[@]}" -p MemoryMax=200G -p CPUQuota=400% /usr/bin/python3 "$repo/gates/overlap_resident_gate.py" build "$d" "$t" "$profile" | \
"${run[@]}" -p MemoryMax=14000M -p CPUQuota=100% /usr/bin/python3 "$repo/gates/overlap_resident_gate.py" oracle "$d" "$t" "$profile"
rc=("${PIPESTATUS[@]}");set -e;printf 'build=%s oracle=%s\n' "${rc[@]}" |tee "$d/exits.log"; [[ ${rc[*]} == '0 0' ]]
python3 gates/overlap_resident_gate.py certify "$d" "$profile"; python3 gates/overlap_ten_score.py "$d"; python3 gates/overlap_resident_report.py "$d" >"$d/summary.md";echo "certified receipt=$d"
