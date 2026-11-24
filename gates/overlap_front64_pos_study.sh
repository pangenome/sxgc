#!/usr/bin/env bash
# Fixed, non-repeating raw→wide-column candidate; the reference/ranker are not re-run.
set -euo pipefail
now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+900 && $1 <= now+1800 )) || exit 75
cd "$(dirname "$0")/..";[[ -f gates/receipts/raw64-p1-10g/score.json && ! -e gates/receipts/pos64-front-10g/score.json ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_front64_pos_study.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$sandbox/pos64-front-10g-*" >/dev/null || exit 75
d=$(mktemp -d "$sandbox/pos64-front-10g-XXXXXX");echo "POS64_STUDY receipt=$d";p=$PWD/gates/overlap_front64_pos_study.py;end=$1
/usr/bin/python3 -BE "$p" admit "$d" "$end"
{ git rev-parse HEAD;g++ --version;g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/front64.cpp -o "$d/front";g++ -std=c++20 -O2 -Wall -Wextra -Werror -fopenmp gates/overlap_front64_position_unit.cpp -o "$d/unit";"$d/unit";/usr/bin/python3 -BE gates/overlap_front64_position_test.py "$d/front"; } >"$d/build-and-test.log" 2>&1
/usr/bin/python3 -BE - "$d" <<'PY'
import hashlib,json,shutil,sys
from pathlib import Path
d=Path(sys.argv[1]);b=Path('gates/receipts/raw64-p1-10g');orig=Path(json.loads((b/'source.json').read_bytes())['original_directory']);h=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();q=orig/'front-verify';expected=json.loads((b/'tools.json').read_bytes())[q.name];assert q.is_file() and not q.is_symlink() and q.stat().st_size<1<<20 and h(q)==expected;shutil.copy2(q,d/q.name);assert h(d/q.name)==expected;(d/'tools.json').write_text(json.dumps({k:h(d/k) for k in ('front','front-verify','unit')},sort_keys=True,indent=2)+'\n')
PY
run(){ local name=$1 mem=$2 cpu=$3 rc;set +e;systemd-run --user --quiet --wait --pipe --collect -p "MemoryMax=$mem" -p MemorySwapMax=0 -p "CPUQuota=$cpu%" -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 /usr/bin/python3 -BE "$p" "$name" "$d" "$end";rc=$?;set -e;echo "$name=$rc" >>"$d/exits.log";((rc==0)) || exit "$rc"; }
run copy 192M 100;run build 32G 400;run verify 14000M 100
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "POS64_STUDY_COMPLETE receipt=$d"
