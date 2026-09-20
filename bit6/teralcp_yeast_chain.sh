#!/bin/bash
# teralcp_yeast_chain.sh — rung 4a.1: continue from the completed yeast235 revlines
# (adopted from the stray prep job), through grlBWT -> rle -> TeraLCP format.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-yeast/grl
GRL=/home/erikg/grlBWT/build/grlbwt-cli
RLE=/home/erikg/grlBWT/build/grlbwt2rle
TL=/home/erikg/TeraTools/src/TeraLCP/TeraLCP
cd "$W"
export TMPDIR="$W/tmp"; mkdir -p "$TMPDIR"

echo "=== waiting for revlines prep to finish (agc2flat pid $1)"
while kill -0 "$1" 2>/dev/null; do sleep 10; done
SZ=$(stat -c%s yeast235.rl.txt)
echo "revlines done: $SZ bytes, sidecar rows: $(wc -l < yeast235.rl.txt.names.tsv)"
[ "$SZ" -gt 3000000000 ] || { echo "ABORT: revlines too small"; exit 1; }

echo "=== STAGE grlbwt ($(date -Is))"
/usr/bin/time -f "grlbwt wall %e s, maxRSS %M KB" \
  $GRL yeast235.rl.txt -t 32 > grl.log 2>&1 || { echo "GRLBWT FAILED"; tail -5 grl.log; exit 1; }
grep -E "Number of runs|BWT size" grl.log | tail -2

echo "=== STAGE rle -> TeraLCP format"
$RLE yeast235.rl_bwt y
mv y.syms y.bwt.heads
mv y.len y.bwt.len
ls -la y.bwt.heads y.bwt.len | awk '{printf "%s: %.2f GB\n", $9, $5/1e9}'

echo "=== STAGE TeraLCP ($(date -Is))"
$TL -h 2>&1 | head -30 || true
echo "=== prep chain COMPLETE $(date -Is)"
