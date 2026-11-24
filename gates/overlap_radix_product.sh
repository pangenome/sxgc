#!/usr/bin/env bash
# Fresh matched raw 1 GB builds. Don't overlap the long 10 GB serial control.
set -euo pipefail
cd "$(dirname "$0")/..";s=/mnt/nvme2n1/erikg/TAPEREADER/sandbox
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
python3 gates/overlap_radix_product_score.py "${receipts[@]}" "$d/comparison.json"
