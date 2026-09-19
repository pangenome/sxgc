#!/bin/bash
# pilot_resume.sh K — resume a pilot after the stream stage completed:
# build -> patterns -> query -> verify over the existing PFP + sidecar.
set -euo pipefail
K=$1
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
W=/mnt/nvme3n1/erikg/sxgc-pilot/k$K
RB=/home/erikg/suffixient-array/build/suff-set-src/rindex_build
RQ=/home/erikg/suffixient-array/build/suff-set-src/rindex_query
TOOLS=/home/erikg/sxgc/tools

dfcheck() {
  local free; free=$(df --output=avail -B1G /mnt/nvme3n1 | tail -1)
  echo "df: ${free}G free on nvme"
  [ "$free" -gt "$1" ] || { echo "ABORT: free below $1G"; exit 1; }
}

cd "$W"
# flatlen from the sidecar (last fstart + len + trailing '$')
FLAT=$(python3 - <<'EOF'
rows = [l.rstrip("\n").split("\t") for l in open("HPRC_r2_assemblies_0.6.1.txt.names.tsv")]
print(max(int(r[1]) + int(r[2]) for r in rows) + 1)
EOF
)
echo "=== PILOT-RESUME k=$K start $(date -Is) flatlen=$FLAT"; dfcheck 300

echo "=== STAGE build: rindex_build (M64, out-of-core)"
rm -f h$K.ri
/usr/bin/time -f "build wall %e s, maxRSS %M KB" \
  $RB -i h$K -w 10 -n $((FLAT+1)) -o h$K.ri > build.log 2>&1 \
  || { echo "STAGE build FAILED"; tail -8 build.log; exit 1; }
grep -E "BWT equal-letter runs|r-index written" build.log
dfcheck 250

echo "=== STAGE patterns"
python3 $TOOLS/pilot_patterns.py --agc "$AGC" --sidecar HPRC_r2_assemblies_0.6.1.txt.names.tsv \
  --n 200 --len 120 --seed 42 --out patterns.fa

echo "=== STAGE query"
/usr/bin/time -f "query wall %e s, maxRSS %M KB" \
  $RQ h$K.ri patterns.fa -o occs.txt -N "$FLAT" > query.log 2>&1 \
  || { echo "STAGE query FAILED"; tail -8 query.log; exit 1; }
tail -2 query.log

echo "=== STAGE verify"
python3 $TOOLS/pilot_verify.py --agc "$AGC" --sidecar HPRC_r2_assemblies_0.6.1.txt.names.tsv \
  --patterns patterns.fa --truth patterns.fa.truth.tsv --occs occs.txt
dfcheck 200
echo "=== PILOT-RESUME k=$K COMPLETE $(date -Is)"
