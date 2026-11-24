#!/usr/bin/env bash
# Single new endpoint: cold raw -> canonical wide columns -> wide ranks; no product or scale continuation.
set -euo pipefail
now=$(date +%s)
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 <= now+1800 && $1 > now+900 )) || exit 75
cd "$(dirname "$0")/..";[[ ! -e gates/receipts/raw64-p1-10g/score.json ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_front64_study.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$s/raw64-p1-10g-*" >/dev/null || exit 75
d=$(mktemp -d "$s/raw64-p1-10g-XXXXXX");echo "RAW64_STUDY receipt=$d";p=$PWD/gates/overlap_front64_study.py;end=$1
/usr/bin/python3 -BE "$p" admit "$d" "$end"
{ git rev-parse HEAD;g++ --version;g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/front64.cpp -o "$d/front";g++ -std=c++20 -O2 -Wall -Wextra -Werror gates/overlap_front64_verify.cpp -lcrypto -o "$d/front-verify";/usr/bin/python3 -BE gates/overlap_front64_test.py "$d/front" "$d/front-verify"; } >"$d/build-and-test.log" 2>&1
/usr/bin/python3 -BE - "$d" <<'PY'
import hashlib,json,shutil,sys
from pathlib import Path
d=Path(sys.argv[1]);b=Path('gates/receipts/parse64-10g');orig=Path(json.loads((b/'source.json').read_bytes())['original_directory']);h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();v={x:h(d/x) for x in ('front','front-verify')}
for x,expected in json.loads((b/'tools.json').read_bytes()).items():
 q=orig/x;assert not q.is_symlink() and q.is_file() and q.stat().st_size<1<<20 and h(q)==expected;shutil.copy2(q,d/x);v[x]=h(d/x);assert v[x]==expected
(d/'tools.json').write_text(json.dumps(v,sort_keys=True,indent=2)+'\n')
PY
run(){ local name=$1 mem=$2 cpu=$3 rc;set +e;systemd-run --user --quiet --wait --pipe --collect -p "MemoryMax=$mem" -p MemorySwapMax=0 -p "CPUQuota=$cpu%" -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 /usr/bin/python3 -BE "$p" "$name" "$d" "$end";rc=$?;set -e;echo "$name=$rc" >>"$d/exits.log";((rc==0)) || exit "$rc"; }
run copy 192M 100;run build 32G 400;run verify 14000M 100
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "RAW64_STUDY_COMPLETE receipt=$d"
