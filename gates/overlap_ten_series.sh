#!/usr/bin/env bash
# Three actual cold builds, not an exhaustive 36-arm grid. Component sweep
# determines native-wide sort threads; the 1 GB product pair isolates batching.
set -euo pipefail
unset PYTHONOPTIMIZE
(( $#==3 )) && [[ $3 =~ ^[1-9][0-9]{0,10}$ ]] || { echo 'usage: overlap_ten_series.sh SERIAL10 PAIR1 DEADLINE_UNIX' >&2;exit 64; }
control=$(realpath "$1"); pair=$(realpath "$2"); deadline=$3
cd "$(dirname "$0")/..";source gates/overlap_pin.sh;overlap_pin_series gates/overlap_ten_series.sh "$control" "$pair" "$deadline"
s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
d=$(mktemp -d "$s/scale-full-10g-XXXXXX");echo "TEN_SERIES receipt=$d source=$PWD"
printf '%s\n' "$control" "$pair" "$deadline" "$(git rev-parse HEAD)" >"$d/schedule.txt"
bash gates/overlap_ten_dict_grid.sh "$control" "$pair" "$deadline" |tee "$d/grid.log"
grid=$(grep '^completed component receipt=' "$d/grid.log");grid=${grid#completed component receipt=}
order=$(python3 -B gates/overlap_scaling_policy.py order "$pair" "$grid");read -ra profiles <<<"$order"
(( ${#profiles[@]} == 3 )); echo "TEN_ORDER $order" |tee "$d/order.log"
# A fixed worker checkout separate from the pinned harness. A study-only
# hashing feature must not replace the product binaries between arms.
tmp=$(mktemp -d /tmp/overlap-ten-series-XXXXXX); src=$tmp/source;tools=$tmp/tools
git clone --quiet --local --shared --no-checkout "$PWD" "$src";git -C "$src" sparse-checkout set xsa gates
git -C "$src" checkout --quiet --detach 8513cccb7d0ade1f4917752e3e8be33c3f7e49e3
bash gates/overlap_ten_product.sh --prepare-tools "$src" "$tools" >"$d/tools.log" 2>&1
printf '%s\n' "$src" "$tools" >"$d/tool-paths.txt"
arms=()
for profile in "${profiles[@]}";do
    # Measured ETA admission, not a proof of completion: leave extra time for
    # copying, independent verification, hashing, receipts and cleanup. The
    # per-arm absolute cooperative deadline is independent of the estimate.
    python3 -B - "$deadline" "$control" "${arms[@]}" <<'PY'
import sys,time,json
from pathlib import Path
secs=[json.loads((Path(p)/'build.json').read_text())['raw_to_product_s'] for p in sys.argv[2:]]
need=(secs[0] if len(secs)==1 else min(secs[0],1.4*max(secs[1:]))) *1.08+480
left=int(sys.argv[1])-time.time();print(f'TEN_ADMISSION remaining_s={left:.1f} need_s={need:.1f}',flush=True)
if left<need:raise SystemExit(75)
PY
    echo "TEN_ARM_START profile=$profile"
    bash gates/overlap_ten_product.sh --profile "$profile" "$src" "$tools" "$deadline" |tee "$d/$profile.log"
    arm=$(grep '^certified receipt=' "$d/$profile.log");arms+=("${arm#certified receipt=}")
    python3 -B gates/overlap_scaling_policy.py score "$control" "$pair" "$grid" "$d/score-${#arms[@]}.json" "${arms[@]}"
    echo "TEN_ARM_CERTIFIED profile=$profile receipt=${arms[-1]}"
done
echo "TEN_SERIES_COMPLETE receipt=$d"
