#!/usr/bin/env bash
# One fixed prepared parse study. Does not open raw text or dispatch another rung.
set -euo pipefail
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 <= 1795531169 && $1 > $(date +%s)+1100 )) || exit 75
cd "$(dirname "$0")/..";[[ ! -f gates/receipts/parse64-10g/score.json ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_parse64_study.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$s/parse64-10g-*" >/dev/null || exit 75
d=$(mktemp -d "$s/parse64-10g-XXXXXX");echo "PARSE64_STUDY receipt=$d";p=$PWD/gates/overlap_parse64_study.py;end=$1
/usr/bin/python3 -BE "$p" admit "$d" "$end"
{ git rev-parse HEAD;g++ --version;g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/parse64.cpp -o "$d/parse64";g++ -std=c++20 -O2 -Wall -Wextra -Werror gates/overlap_parse64_verify.cpp -o "$d/verify"; /usr/bin/python3 -BE gates/overlap_parse64_test.py "$d/parse64" "$d/verify"; } >"$d/build-and-test.log" 2>&1
/usr/bin/python3 -BE - "$d" <<'PY'
import hashlib,json,sys
from pathlib import Path
d=Path(sys.argv[1]);(d/'tools.json').write_text(json.dumps({k:hashlib.sha256((d/k).read_bytes()).hexdigest() for k in ['parse64','verify']},sort_keys=True,indent=2)+'\n')
PY
run(){ local name=$1 mem=$2 cpu=$3 rc;set +e;systemd-run --user --quiet --wait --pipe --collect -p "MemoryMax=$mem" -p MemorySwapMax=0 -p "CPUQuota=$cpu%" -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 /usr/bin/python3 -BE "$p" "$name" "$d" "$end";rc=$?;set -e;echo "$name=$rc" >>"$d/exits.log";((rc==0)) || exit "$rc"; }
run copy 192M 100;run lift 24G 400;run keys 192M 100;run rank4 24G 400;run verify4 14000M 100;run rank1 24G 400;run verify1 14000M 100
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "PARSE64_STUDY_COMPLETE receipt=$d"
