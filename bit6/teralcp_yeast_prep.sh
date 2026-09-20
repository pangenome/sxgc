#!/bin/bash
# teralcp_yeas1_prep.sh — rung 4a.1 prep: yeast235 RLBWT via grlBWT, in TeraLCP rlbwt format.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-pilot/teralcp-yeast
AGC=/home/erikg/yeast/yeast235.agc
A2F=/home/erikg/sxgc/agc2flat/target/release/agc2flat
GRL=/home/erikg/grlBWT/build/grlbwt-cli
RLE=/home/erikg/grlBWT/build/grlbwt2rle
mkdir -p "$W/tmp"; cd "$W"
export TMPDIR="$W/tmp"

echo "=== STAGE revlines: yeast235 whole collection"
$A2F "$AGC" --revlines -o yeast235.revlines 2> prep.log
tail -2 prep.log

echo "=== STAGE grlbwt"
/usr/bin/time -f "grlbwt wall %e s, maxRSS %M KB" \
  $GRL yeast235.revlines -t 32 > grl.log 2>&1 || { echo "GRLBWT FAILED"; tail -5 grl.log; exit 1; }
grep -E "Number of runs|BWT size" grl.log | tail -2 || tail -3 grl.log

echo "=== STAGE rle + rename to TeraLCP format"
$RLE yeast235.rl_bwt y
mv y.syms y.bwt.heads
mv y.len y.bwt.len
ls -la y.bwt.heads y.bwt.len | awk '{printf "%s: %.2f GB\n", $9, $5/1e9}'
echo "=== PREP COMPLETE $(date -Is)"
