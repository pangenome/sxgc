#!/usr/bin/env bash
# Fresh matched raw 1 GB builds. Don't overlap the long 10 GB serial control.
set -euo pipefail
cd "$(dirname "$0")/..";s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
case $#:${1:-} in 0:) batch=0;; 1:--include-batch) batch=1;; *) echo 'usage: overlap_radix_product.sh [--include-batch]' >&2;exit 64;;esac
source gates/overlap_pin.sh
overlap_pin_series gates/overlap_radix_product.sh "$@"
if ((batch));then python3 - <<'PY'
import json
from pathlib import Path
r=json.loads(Path('gates/receipts/sxls-pair-100m/result.json').read_text());assert r['slices']==382 and r['observed_saved_syscw']==r['predicted_saved_syscw'] and r['local_wall_speedup']>1
PY
fi
lanes=$(python3 - <<'PY'
from pathlib import Path
import json
for cmd in Path('/proc').glob('[0-9]*/cmdline'):
    try:
        a=cmd.read_bytes().split(b'\0')
    except OSError:continue
    if b'packed-10g-bitmap' in a and b'build' in a and any(x.endswith(b'/overlap_resident_gate.py') for x in a):raise SystemExit('defer matched product timing until 10 GB control exits')
r=json.loads(Path('gates/receipts/radix-emit-100m/result.json').read_text());t=r['emit_s'];fast=min(t['two'],t['four']);limit=min(r['serial_emit_s']);assert fast<limit
# If practically tied, leave CPU headroom for the sink/finish.
print(2 if t['two']<=1.10*fast and t['two']<limit else 4)
PY
)
d=$(mktemp -d "$s/radix-full-pair-XXXXXX");echo "pair=$d lanes=$lanes";declare -a receipts
for n in 1 "$lanes";do
    bash gates/overlap_resident_gate.sh "packed-radix-$n" |tee "$d/arm-$n.log"
    arm=$(grep '^certified receipt=' "$d/arm-$n.log"); receipts+=("${arm#certified receipt=}")
done
rc=0;python3 gates/overlap_radix_product_score.py "${receipts[@]}" "$d/comparison.json" || rc=$?
# A verified speed miss isn't a construction failure. Keep its score while
# isolating the next independent writer change on the certified lane stream.
if ((rc!=0 && rc!=75));then exit "$rc";fi
if ((batch));then
    bash gates/overlap_resident_gate.sh "packed-radix-$lanes-batch" |tee "$d/arm-$lanes-batch.log"
    arm=$(grep '^certified receipt=' "$d/arm-$lanes-batch.log");brc=0
    python3 gates/overlap_radix_product_score.py --batch "${receipts[1]}" "${arm#certified receipt=}" "$d/batch.json" || brc=$?
    if ((brc!=0));then exit "$brc";fi
fi
exit "$rc"
