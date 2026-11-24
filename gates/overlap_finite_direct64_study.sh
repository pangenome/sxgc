#!/usr/bin/env bash
# One new direct-range arm; no rerun of the completed coarse control, adapter or rank proof.
set -euo pipefail
now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+600 && $1 <= now+1000 )) || exit 75
cd "$(dirname "$0")/..";[[ -f gates/receipts/finite-emit64-100m/score.json && ! -e gates/receipts/finite-direct64-100m ]] || exit 75
source gates/overlap_pin.sh;overlap_pin_series gates/overlap_finite_direct64_study.sh "$1"
exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$sandbox/finite-direct64-100m-*" >/dev/null || exit 75
d=$(mktemp -d "$sandbox/finite-direct64-100m-XXXXXX");echo "FINITE_DIRECT64_STUDY receipt=$d";p=$PWD/gates/overlap_finite_direct64_study.py;end=$1
trap 'rc=$?;echo "launcher=$rc" >>"$d/exits.log";if ((rc));then touch "$d/stop.requested";fi' EXIT
/usr/bin/python3 -BE "$p" admit "$d" "$end"
{ git rev-parse HEAD;g++ --version;g++ -std=c++20 -O3 -Wall -Wextra -Werror -fopenmp xsa/pfp/tools/finite_emit64.cpp -lcrypto -o "$d/emit";python3 -BE gates/overlap_finite_direct64_test.py "$d/emit";"$d/emit" sampled plan 100000503 3170531 2689467 95434516 38400 192001 4; } >"$d/build-and-test.log" 2>&1
/usr/bin/python3 -BE - "$d" <<'PY'
import shutil,sys
from pathlib import Path
sys.path.insert(0,'gates');import overlap_finite_direct64_study as g
d=Path(sys.argv[1]);src=Path(g.load(g.BANK,'source.json')['original_directory'])/'oracle';assert src.is_file() and src.resolve()==src and g.s.sha(src)==g.CHECKER;shutil.copyfile(src,d/'oracle');(d/'oracle').chmod(0o500);g.s.save(d/'tools.json',{k:g.s.sha(d/k) for k in ('emit','oracle')});g.tools(d)
PY
systemd-run --user --quiet --wait --pipe --collect -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 -p RuntimeMaxSec=120 /usr/bin/python3 -BE "$p" setup "$d" "$end"
echo setup=0 >>"$d/exits.log";/usr/bin/python3 -BE "$p" pair "$d" "$end";echo pair4=0 >>"$d/exits.log"
/usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "FINITE_DIRECT64_STUDY_COMPLETE receipt=$d"
