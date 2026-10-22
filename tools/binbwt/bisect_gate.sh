#!/bin/bash
# solB gate: 10 GB pile snap through the entry, detached, logged to /mnt/nvme2n1/erikg/solB-<TAG>.log
# Usage: bisect_gate.sh <TAG> <ABS-XSA-BINARY> [MEMORY_GB=55]
# Verdict classifier (per 2026-10-22 steering):
#   PASS      = entry RC=0 (chi banked in-process; product sha'd vs bank)
#   ENV-KILL  = phase rc=137 (SIGKILL, OOM weather), peak RSS 60-80 GB band, no fault line
#   CODE-FAIL = any other rc / fault signature (assert, mismatch, segv, RC=139...)
set -u
TAG="$1"; X="$2"; MEM="${3:-55}"
WT=/home/erikg/worktrees/sxgc/pi-worktree-e4547987-d9e4-4411-b7b0-bb0341d81b87-s0-0
LOG=/mnt/nvme2n1/erikg/solB-$TAG.log
S=/mnt/nvme2n1/erikg/solB-$TAG-scratch
mkdir -p "$S"
nohup bash -c "
{
echo '=== solB gate $TAG mem_gb=$MEM start ' \$(date -Is) ' epoch=' \$(date +%s)
echo 'binary=$X sha256=' \$(sha256sum $X | cut -d' ' -f1)
echo 'tree=' \$(git -C $WT rev-parse HEAD)
echo 'branch=' \$(git -C $WT branch --show-current)
rm -rf $S
mkdir -p $S
$X build --input /mnt/nvme2n1/erikg/extcols-scratch/real10b-ref/pile-10b-snap.txt --scratch $S --remap /mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin --threads 96 --memory-gb $MEM --finish-as-gb 400 --wit-ram-gb 8 --chi-expect 2676929430 --scratch-free-pct 8 --timeout 2147483647
RC=\$?
echo \"GATE_$TAG RC=\$RC end=\$(date +%s)\"
grep -E 'PHASE .* (END|rc=)' $S/xsa-build.log | tail -6
tail -3 $S/xsa-build.log
if [ \"\$RC\" = \"0\" ]; then
  for f in \$(find $S -maxdepth 2 -name '*.sxi2' -type f); do
    echo \"PRODUCT \$(basename \$f) sha256=\$(sha256sum \$f | cut -d' ' -f1) bytes=\$(stat -c%s \$f)\"
  done
  echo \"BANK sha256=\$(sha256sum /mnt/nvme2n1/erikg/pf2-10g-hyg0/product.sxi2 | cut -d' ' -f1) bytes=\$(stat -c%s /mnt/nvme2n1/erikg/pf2-10g-hyg0/product.sxi2)\"
fi
} >> $LOG 2>&1
" >/dev/null 2>&1 &
echo "launched gate $TAG (mem=$MEM) log=$LOG scratch=$S pid=$!"
