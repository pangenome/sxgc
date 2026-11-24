#!/usr/bin/env bash
# Exact-width census first, under the separate standalone front-end grant.
set -euo pipefail
cd "$(dirname "$0")/.."; repo=$PWD; root=/mnt/nvme2n1/erikg
python3 - <<'PY'
import json,pathlib
p=pathlib.Path('gates/receipts/packed-3g-bitmap'); get=lambda s:json.loads((p/s).read_text()); assert get('identity.json')['verified'] and get('goals.json')['matched_bitmap_pass'] and get('goals.json')['promotion_under_3x']
PY
T=$(mktemp -d /tmp/lexparse-ten-XXXXXX); d=$(mktemp -d "$root/TAPEREADER/sandbox/packed-front-10g-XXXXXX"); echo "receipt=$d tools=$T"
g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_lexparse10.cpp -o "$T/lexparse10"
python3 gates/overlap_ten_front.py test "$T/lexparse10" |tee "$d/fixtures.log"
mkdir "$d/in"; test "$(stat -c%s "$root/pf-10gb.txt")" = 10000000000
cp --reflink=never "$root/pf-10gb.txt" "$d/in/corpus"; { git rev-parse HEAD; git status --porcelain; sha256sum "$T/lexparse10"; } >"$d/binaries.sha"
python3 gates/private_cold.py "$d/in/corpus"; echo 'private cold 10000000000 B corpus, mincore zero pages' |tee "$d/cold.log"
systemd-run --user --quiet --collect --wait --pipe -p MemorySwapMax=0 -p MemoryMax=192M -p CPUQuota=100% -p Nice=10 -p LimitNOFILE=8192 /usr/bin/python3 "$repo/gates/overlap_ten_front.py" build "$T/lexparse10" "$d"
echo "10 GB census complete (not product): $d"
