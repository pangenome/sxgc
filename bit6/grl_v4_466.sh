#!/bin/bash
# grl_v4_466.sh — FULL HPRC v2 (466 haplotypes, ~1.44 Tbp) via the adopted
# substrate chain. 96-core policy for every stage (shared-box citizenship):
# grlBWT -t 96, OMP_NUM_THREADS=96 for TeraLCP, teralcp_chi -t 96.
# The 1.44 TB revlines file is a one-time transient input for grlBWT
# (seekable-input requirement); it can be deleted once the BWT exists.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-pilot/k466
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
SAMPLES=/home/erikg/sxgc/bit6-466.samples.txt
A2F=/home/erikg/sxgc/agc2flat/target/release/agc2flat
GRL=/home/erikg/grlBWT/build/grlbwt-cli
RLE=/home/erikg/grlBWT/build/grlbwt2rle
TLC=/home/erikg/TeraTools/src/TeraLCP/TeraLCP
CHI=/tmp/grl_gate/teralcp_chi
RQ=/home/erikg/suffixient-array/build/suff-set-src/rindex_query
TOOLS=/home/erikg/sxgc/tools
NTHREADS=96
mkdir -p "$W"; cd "$W"
export TMPDIR="$W/tmp"; mkdir -p "$W/tmp"
export OMP_NUM_THREADS=$NTHREADS

df -h /mnt/nvme3n1 | tail -1

echo "=== STAGE prep: revlines collection from AGC ($(date -Is))"
if [ ! -f h466_rl.txt ]; then
  $A2F "$AGC" --samples "$SAMPLES" --revlines -o h466_rl.txt 2> prep.log \
    || { echo "PREP FAILED"; tail -5 prep.log; exit 1; }
fi
tail -2 prep.log
df -h /mnt/nvme3n1 | tail -1

echo "=== STAGE grlbwt -t $NTHREADS ($(date -Is))"
if [ ! -f h466_rl.rl_bwt ]; then
  /usr/bin/time -f "grlbwt wall %e s, maxRSS %M KB" \
    $GRL h466_rl.txt -t $NTHREADS -T "$TMPDIR" > grl.log 2>&1 \
    || { echo "GRLBWT FAILED"; tail -5 grl.log; exit 1; }
fi
grep -E "Number of runs|BWT size" grl.log | tail -2 || tail -3 grl.log
echo "(revlines consumed; may now be deleted to reclaim 1.44 TB if desired)"

echo "=== STAGE rle + 5-byte convert ($(date -Is))"
$RLE h466_rl.rl_bwt h466r
python3 - <<'EOF'
import struct
R = len(open('h466r.syms','rb').read())
vals = struct.unpack(f'<{R}I', open('h466r.len','rb').read()[:R*4])
open('h466rt.bwt.heads','wb').write(open('h466r.syms','rb').read())
with open('h466rt.bwt.len','wb') as f:
    for v in vals: f.write(v.to_bytes(5,'little'))
open('h466rt.scratch','wb').close()
print('h466 rlbwt: R=%d n=%d' % (R, sum(vals)))
EOF

echo "=== STAGE TeraLCP ($(date -Is))"
if [ ! -f h466rt.lcp_index.lcp_index ]; then
  /usr/bin/time -f "teralcp wall %e s, maxRSS %M KB" \
    $TLC -f rlbwt -i h466rt -t h466rt.scratch -oindex h466rt.lcp_index -v quiet 2> tlc.log \
    || { echo "TERALCP FAILED"; tail -5 tlc.log; exit 1; }
fi
ls -la h466rt.lcp_index.lcp_index | awk '{printf "index: %.1f GB\n", $5/1e9}'

echo "=== STAGE chi + samples -t $NTHREADS ($(date -Is))"
if [ ! -f h466.ri4 ]; then
  /usr/bin/time -f "chi wall %e s, maxRSS %M KB" \
    $CHI h466rt.lcp_index.lcp_index --rlbwt h466rt --sidecar h466_rl.txt.names.tsv \
       --samples h466.ri4 -o chi_h466.sA -t $NTHREADS 2> chi.log \
    || { echo "CHI FAILED"; tail -5 chi.log; exit 1; }
fi
tail -3 chi.log

echo "=== STAGE patterns: 200 oracle-planted forward 120-mers ($(date -Is))"
python3 $TOOLS/pilot_patterns.py --agc "$AGC" --sidecar h466_rl.txt.names.tsv \
  --n 200 --len 120 --seed 42 --out patterns.fa

echo "=== STAGE query ($(date -Is))"
/usr/bin/time -f "query wall %e s, maxRSS %M KB" \
  $RQ h466.ri4 patterns.fa -o h466_occs.txt 2> query.log \
  || { echo "QUERY FAILED"; tail -5 query.log; exit 1; }
tail -2 query.log

echo "=== STAGE verify ($(date -Is))"
python3 $TOOLS/pilot_verify.py --agc "$AGC" --sidecar h466_rl.txt.names.tsv \
  --patterns patterns.fa --truth patterns.fa.truth.tsv --occs h466_occs.txt 2>&1 | tail -3

echo "=== H466 CHAIN COMPLETE $(date -Is)"
