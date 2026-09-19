#!/bin/bash
# grl_v4_k10.sh — complete the grlBWT v4 pipeline on k=10 from the salvaged BWT:
# rle-convert -> parallel LF sampler -> v4 query -> oracle byte-verification.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-pilot/k10
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
TSC=$W/h10_rl.txt.names.tsv
RQ=/home/erikg/suffixient-array/build/suff-set-src/rindex_query
TOOLS=/home/erikg/sxgc/tools
cd "$W"

echo "=== STAGE salvage: /tmp BWT -> h10_rl.rl_bwt"
if [ ! -f h10_rl.rl_bwt ]; then
  SRC=$(ls /tmp/grl.bwt.*/bwt_lev_0_* | head -1)
  mv "$SRC" h10_rl.rl_bwt
  rm -rf /tmp/grl.bwt.* || true
fi
ls -la h10_rl.rl_bwt | awk '{printf "rl_bwt: %.2f GB\n", $5/1e9}'

echo "=== STAGE rle: grlbwt2rle"
/home/erikg/grlBWT/build/grlbwt2rle h10_rl.rl_bwt h10r
ls -la h10r.syms h10r.len | awk '{printf "%s: %.2f GB\n", $9, $5/1e9}'

echo "=== STAGE sampler: parallel LF walks ($(date -Is))"
/usr/bin/time -f "sampler wall %e s, maxRSS %M KB" \
  /home/erikg/sxgc/bit6/rlbwt_sampler h10r "$TSC" h10.ri4 128 2> sampler.log \
  || { echo "SAMPLER FAILED"; tail -5 sampler.log; exit 1; }
tail -2 sampler.log
ls -la h10.ri4 | awk '{printf "h10.ri4: %.2f GB\n", $5/1e9}'

echo "=== STAGE patterns: 200 oracle-planted forward 120-mers"
python3 $TOOLS/pilot_patterns.py --agc "$AGC" --sidecar "$TSC" --n 200 --len 120 --seed 42 --out patterns.fa

echo "=== STAGE query"
/usr/bin/time -f "query wall %e s, maxRSS %M KB" \
  $RQ h10.ri4 patterns.fa -o grl_occs.txt 2> query.log \
  || { echo "QUERY FAILED"; tail -5 query.log; exit 1; }
tail -2 query.log

echo "=== STAGE verify"
python3 $TOOLS/pilot_verify.py --agc "$AGC" --sidecar "$TSC" \
  --patterns patterns.fa --truth patterns.fa.truth.tsv --occs grl_occs.txt
echo "=== GRL v4 k=10 COMPLETE $(date -Is)"
