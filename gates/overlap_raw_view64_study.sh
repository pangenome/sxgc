#!/usr/bin/env bash
# Raw + previously certified inverse, 10 GB text, one 4→1 matched view pair, no front/sorter/product rerun.
set -euo pipefail
now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+1600 && $1 <= now+2400 )) || exit 75
cd "$(dirname "$0")/..";[[ -f gates/receipts/pos64-front-10g/score.json && ! -e gates/receipts/raw-view64-10g/score.json ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_raw_view64_study.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$sandbox/raw-view64-10g-*" >/dev/null || exit 75
d=$(mktemp -d "$sandbox/raw-view64-10g-XXXXXX");echo "RAW_VIEW64_STUDY receipt=$d";p=$PWD/gates/overlap_raw_view64_study.py;end=$1
/usr/bin/python3 -BE "$p" admit "$d" "$end"
{ git rev-parse HEAD;g++ --version;g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/raw_rank_view64.cpp -lcrypto -o "$d/view";g++ -std=c++20 -O2 -Wall -Wextra -Werror gates/overlap_raw_view64_oracle.cpp -lcrypto -o "$d/oracle";/usr/bin/python3 -BE gates/overlap_raw_view64_test.py "$d/view" "$d/oracle"; } >"$d/build-and-test.log" 2>&1
/usr/bin/python3 -BE - "$d" <<'PY'
import hashlib,json,sys
from pathlib import Path
d=Path(sys.argv[1]);(d/'tools.json').write_text(json.dumps({k:hashlib.sha256((d/k).read_bytes()).hexdigest() for k in ('view','oracle')},sort_keys=True,indent=2)+'\n')
PY
run(){ local name=$1 mem=$2 cpu=$3 rc;set +e;systemd-run --user --quiet --wait --pipe --collect -p "MemoryMax=$mem" -p MemorySwapMax=0 -p "CPUQuota=$cpu%" -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 /usr/bin/python3 -BE "$p" "$name" "$d" "$end";rc=$?;set -e;echo "$name=$rc" >>"$d/exits.log";((rc==0)) || exit "$rc"; }
run copy 192M 100;run view4 20G 400;run view1 20G 400;run witness 192M 100;run oracle 14000M 100
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "RAW_VIEW64_STUDY_COMPLETE receipt=$d"
