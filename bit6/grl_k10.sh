#!/bin/bash
# grl_k10.sh — grlBWT route on the k=10 HPRC pilot set (for the differential
# comparison vs the PFP route): revlines prep from AGC -> grlbwt-cli -> stats.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-pilot/k10
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
A2F=/home/erikg/sxgc/agc2flat/target/release/agc2flat
GRL=/home/erikg/grlBWT/build/grlbwt-cli
cd "$W"
export TMPDIR="$W/tmp"
mkdir -p "$TMPDIR"   # grlBWT renames its temp output -> must share a filesystem with $W

echo "=== STAGE prep: revlines collection from AGC ($(date -Is))"
/usr/bin/time -f "prep wall %e s, maxRSS %M KB" \
  $A2F "$AGC" --samples samples.txt --revlines -o h10_rl.txt 2> prep.log \
  || { echo "PREP FAILED"; tail -5 prep.log; exit 1; }
tail -2 prep.log; df --output=avail -B1G /mnt/nvme3n1 | tail -1

echo "=== STAGE grlbwt: BCR BWT construction ($(date -Is))"
rm -f h10_rl.rl_bwt
/usr/bin/time -f "grlbwt wall %e s, maxRSS %M KB" \
  $GRL h10_rl.txt -t 32 > grl.log 2>&1 \
  || { echo "GRLBWT FAILED"; tail -8 grl.log; exit 1; }
tail -4 grl.log

echo "=== STAGE stats"
/home/erikg/grlBWT/build/bwt_stats h10_rl.rl_bwt 2>&1 | head -6 || true
ls -la h10_rl.rl_bwt | awk '{printf "rl_bwt: %.2f GB\n", $5/1e9}'
df --output=avail -B1G /mnt/nvme3n1 | tail -1
echo "=== GRL k=10 COMPLETE $(date -Is)"
