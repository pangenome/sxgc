#!/bin/bash
# k10 memory-gate: two isolated input copies; stock oracle (supplies .ssa_t) vs memory build;
# byte-compare BOTH against the retained accepted h10ss_pfp outputs and each other on .ssa_t.
# The retained /mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp.* files are NEVER written.
set -euo pipefail
R=/home/erikg/sxgc
W=$R/bit6/sxi_logs/parse-memory/work-main
K10=/mnt/nvme3n1/erikg/sxgc-pilot/k10
LOG=$W/k10-gate.log
cd "$W"
echo "=== prep input copies $(date -u) ===" >> $LOG
mkdir -p stock-input mem-input stock-tmp mem-tmp
# map retained h10ss_pfp files onto the 'parse' prefix rpfbwt expects
for pair in "parse parse.parse" "dict parse.dict" "parse.parse parse.parse.parse" "parse.dict parse.parse.dict"; do
  set -- $pair
  [ -f "stock-input/$2" ] || cp "$K10/h10ss_pfp.$1" "stock-input/$2"
  [ -f "mem-input/$2" ]   || cp "$K10/h10ss_pfp.$1" "mem-input/$2"
done
echo "=== stock rpfbwt (oracle incl .ssa_t) $(date -u) ===" >> $LOG
/usr/bin/time -v -o k10-stock.time /usr/bin/prlimit --as=350000000000 \
  "$R/sealed-tools/rpfbwt" --l1-prefix "$W/stock-input/parse" --w1 10 --w2 5 \
  --threads 48 --chunks 50 --tmp-dir "$W/stock-tmp" >> $LOG 2>&1
for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do
  cmp "$W/stock-input/parse$ext" "$K10/h10ss_pfp$ext" 2>/dev/null \
    && echo "stock-vs-retained $ext BYTE_IDENTICAL" >> $LOG \
    || echo "stock-vs-retained $ext DIFFERS-or-absent (retained has no .ssa_t: expected for that one)" >> $LOG
done
echo "=== memory rpfbwt $(date -u) ===" >> $LOG
/usr/bin/time -v -o k10-memory.time /usr/bin/prlimit --as=350000000000 \
  "$W/rpfbwt-memory" --l1-prefix "$W/mem-input/parse" --w1 10 --w2 5 \
  --threads 48 --chunks 50 --tmp-dir "$W/mem-tmp" >> $LOG 2>&1
PASS=1
for ext in .rlebwt .rlebwt.meta .ssa .ssa_t; do
  if cmp -s "$W/mem-input/parse$ext" "$W/stock-input/parse$ext"; then
    echo "memory-vs-stock $ext BYTE_IDENTICAL" >> $LOG
  else
    echo "memory-vs-stock $ext MISMATCH" >> $LOG; PASS=0
  fi
done
grep -E "Elapsed|Maximum resident" k10-stock.time k10-memory.time >> $LOG
echo "K10_MEMORY_GATE: $([ $PASS = 1 ] && echo PASS || echo FAIL) $(date -u)" >> $LOG
tail -3 $LOG
