#!/usr/bin/env bash
# Matched prepared 100 MB component arms only; independent checker consumes FIFO in a separate cgroup.
set -euo pipefail
now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+900 && $1 <= now+1500 )) || exit 75
cd "$(dirname "$0")/..";[[ -f gates/receipts/raw-view64-10g/score.json && ! -e gates/receipts/finite-emit64-100m ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_finite_emit64_study.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$sandbox/finite-emit64-100m-*" >/dev/null || exit 75
d=$(mktemp -d "$sandbox/finite-emit64-100m-XXXXXX");echo "FINITE_EMIT64_STUDY receipt=$d";p=$PWD/gates/overlap_finite_emit64_study.py;end=$1
trap 'rc=$?;echo "launcher=$rc" >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
/usr/bin/python3 -BE "$p" admit "$d" "$end"
{ git rev-parse HEAD;g++ --version;g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/finite_emit64.cpp -lcrypto -o "$d/emit";g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_bounded_component_oracle.cpp -lcrypto -o "$d/oracle";g++ -std=c++17 -O2 -Wall -Wextra -Werror gates/overlap_parse64_verify.cpp -o "$d/verify";python3 -BE gates/overlap_finite_emit64_test.py "$d/emit";"$d/emit" plan 100000503 3170531 2689467 95434516 2049 192001 4; } >"$d/build-and-test.log" 2>&1
/usr/bin/python3 -BE - "$d" <<'PY'
import hashlib,json,sys
from pathlib import Path
d=Path(sys.argv[1]);(d/'tools.json').write_text(json.dumps({k:hashlib.sha256((d/k).read_bytes()).hexdigest() for k in ('emit','verify','oracle')},sort_keys=True,indent=2)+'\n')
PY
systemd-run --user --quiet --wait --pipe --collect -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 -p RuntimeMaxSec=120 /usr/bin/python3 -BE "$p" setup "$d" "$end"
echo setup=0 >>"$d/exits.log"
for w in 4 1;do /usr/bin/python3 -BE "$p" pair "$d" "$end" "$w";echo "pair$w=0" >>"$d/exits.log";done
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "FINITE_EMIT64_STUDY_COMPLETE receipt=$d"
