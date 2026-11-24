# Sourced by the two fixed sampled-range follow-ups. The launcher identity and deadline stay
# on argv for the lifetime of the timing lease. No corpus path, binary or grant overrides.
finite_treatment() {
    local mode=$1 study control label now end sandbox d p trappath;shift
    case "$mode" in
        tiled) study=tiles64;control=direct64;label=FINITE_TILES64_STUDY;;
        cached) study=cache64;control=tiles64;label=FINITE_CACHE64_STUDY;;
        *) exit 75;;
    esac
    now=$(date +%s);[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1 > now+600 && $1 <= now+1000 )) || exit 75
    end=$1;cd "$(dirname "${BASH_SOURCE[0]}")/..";[[ -f gates/receipts/finite-$control-100m/score.json && ! -e gates/receipts/finite-$study-100m ]] || exit 75
    source gates/overlap_pin.sh;overlap_pin_series "gates/overlap_finite_${study}_study.sh" "$end"
    exec 9>"${XDG_RUNTIME_DIR:?}/overlap-ten-timing.lock";flock -n 9 || exit 75
    sandbox=/mnt/nvme2n1/erikg/TAPEREADER/sandbox;! compgen -G "$sandbox/finite-$study-100m-*" >/dev/null || exit 75
    d=$(mktemp -d "$sandbox/finite-$study-100m-XXXXXX");echo "$label receipt=$d";p=$PWD/gates/overlap_finite_${study}_study.py
    # Capture the path now: d is local to this function when the shell's EXIT trap runs.
    printf -v trappath '%q' "$d";trap 'rc=$?;echo "launcher=$rc" >>'"$trappath"'/exits.log;if ((rc));then touch '"$trappath"'/stop.requested;fi' EXIT
    /usr/bin/python3 -BE "$p" admit "$d" "$end"
    { git rev-parse HEAD;g++ --version;local -a flags=(-std=c++20 -O3 -Wall -Wextra -Werror -fopenmp);g++ "${flags[@]}" xsa/pfp/tools/finite_emit64.cpp -lcrypto -o "$d/emit"
      if [[ $mode == cached ]];then g++ "${flags[@]}" gates/overlap_finite_cache64_unit.cpp -lcrypto -o "$d/order-test";"$d/order-test";fi
      python3 -BE gates/overlap_finite_tiles64_test.py "$d/emit" "$mode";"$d/emit" "$mode" plan 100000503 3170531 2689467 95434516 38400 192001 4; } >"$d/build-and-test.log" 2>&1
    /usr/bin/python3 -BE - "$d" <<'PY'
import shutil,sys
from pathlib import Path
sys.path.insert(0,'gates');import overlap_finite_direct64_study as g
d=Path(sys.argv[1]);src=Path(g.load(g.BANK,'source.json')['original_directory'])/'oracle';assert src.is_file() and src.resolve()==src and g.s.sha(src)==g.CHECKER;shutil.copyfile(src,d/'oracle');(d/'oracle').chmod(0o500);g.s.save(d/'tools.json',{k:g.s.sha(d/k) for k in ('emit','oracle')});g.tools(d)
PY
    local -a unit=(systemd-run --user --quiet --wait --pipe --collect -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p Nice=10 -p IOAccounting=yes -p LimitCORE=0 -p RuntimeMaxSec=120 /usr/bin/python3 -BE "$p")
    "${unit[@]}" setup "$d" "$end";echo setup=0 >>"$d/exits.log"
    /usr/bin/python3 -BE "$p" pair "$d" "$end";echo pair4=0 >>"$d/exits.log"
    "${unit[@]}" identity "$d" "$end";echo identity=0 >>"$d/exits.log"
    /usr/bin/python3 -BE "$p" score "$d" >"$d/score.json";echo "${label}_COMPLETE receipt=$d"
}
