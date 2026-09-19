#!/bin/bash
# pilot_run.sh K SAMPLES_FILE — staged HPRC haplotype pilot.
# Zero materialization: agc2flat --samples --reverse --stdout | pscan -S,
# out-of-core rindex_build, oracle-served pattern gate. Scratch stays on
# /mnt/nvme3n1; df guards between stages.
set -euo pipefail
K=$1; SAMPLES=$2
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
W=/mnt/nvme3n1/erikg/sxgc-pilot/k$K
A2F=/home/erikg/sxgc/agc2flat/target/release/agc2flat
PSCAN=/home/erikg/suffixient-array/build/pfp-src/pscan
RB=/home/erikg/suffixient-array/build/suff-set-src/rindex_build
RQ=/home/erikg/suffixient-array/build/suff-set-src/rindex_query
TOOLS=/home/erikg/sxgc/tools

dfcheck() {
  local free; free=$(df --output=avail -B1G /mnt/nvme3n1 | tail -1)
  echo "df: ${free}G free on nvme"
  [ "$free" -gt "$1" ] || { echo "ABORT: free below $1G"; exit 1; }
}

mkdir -p "$W"; cd "$W"
cp "$SAMPLES" samples.txt
echo "=== PILOT k=$K start $(date -Is)"; dfcheck 400

echo "=== STAGE stream: agc2flat --samples | pscan -S"
$A2F "$AGC" --samples samples.txt --reverse --stdout 2> a2f.log \
  | $PSCAN h$K -S -w 10 -p 100 -t 1 -s > pscan.log 2>&1 \
  || { echo "STAGE stream FAILED"; tail -5 pscan.log a2f.log; exit 1; }
FLAT=$(grep -o 'Total input symbols: [0-9]*' pscan.log | grep -o '[0-9]*' | head -1)
[ -n "$FLAT" ] || { echo "ABORT: no 'Total input symbols' in pscan.log"; tail -20 pscan.log; exit 1; }
grep -E "Total number of words|Found .* distinct" pscan.log || true
echo "STAGE stream DONE: flatlen=$FLAT ($(grep -c . samples.txt) haps)"; dfcheck 300

echo "=== STAGE build: rindex_build (out-of-core)"
/usr/bin/time -f "build wall %e s, maxRSS %M KB" \
  $RB -i h$K -w 10 -n $((FLAT+1)) -o h$K.ri > build.log 2>&1 \
  || { echo "STAGE build FAILED"; tail -8 build.log; exit 1; }
grep -E "BWT equal-letter runs|r-index written" build.log
dfcheck 250

echo "=== STAGE patterns: oracle-sampled (n=200, len=120, seed=42)"
TSC=$W/HPRC_r2_assemblies_0.6.1.txt.names.tsv
python3 $TOOLS/pilot_patterns.py --agc "$AGC" --sidecar "$TSC" --n 200 --len 120 --seed 42 --out patterns.fa

echo "=== STAGE query"
/usr/bin/time -f "query wall %e s, maxRSS %M KB" \
  $RQ h$K.ri patterns.fa -o occs.txt -N "$FLAT" > query.log 2>&1 \
  || { echo "STAGE query FAILED"; tail -8 query.log; exit 1; }
tail -2 query.log

echo "=== STAGE verify: oracle byte-verify + planted-truth recall"
python3 $TOOLS/pilot_verify.py --agc "$AGC" --sidecar "$TSC" \
  --patterns patterns.fa --truth patterns.fa.truth.tsv --occs occs.txt
dfcheck 200
echo "=== PILOT k=$K COMPLETE $(date -Is)"
