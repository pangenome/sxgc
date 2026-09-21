#!/bin/bash
# grl_v4_k50.sh — k=50 human rung via the adopted substrate chain:
# revlines prep from AGC -> grlBWT -> rle/5-byte -> TeraLCP -> teralcp_chi
# (chi + v4 samples in one O(r)-space pass, optimized walk) -> planted
# patterns -> v4 query -> AGC oracle byte-verification.
set -euo pipefail
W=/mnt/nvme3n1/erikg/sxgc-pilot/k50
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
SAMPLES=/home/erikg/sxgc/bit6-k50.samples.txt
A2F=/home/erikg/sxgc/agc2flat/target/release/agc2flat
GRL=/home/erikg/grlBWT/build/grlbwt-cli
RLE=/home/erikg/grlBWT/build/grlbwt2rle
TLC=/home/erikg/TeraTools/src/TeraLCP/TeraLCP
CHI=/tmp/grl_gate/teralcp_chi
RQ=/home/erikg/suffixient-array/build/suff-set-src/rindex_query
TOOLS=/home/erikg/sxgc/tools
mkdir -p "$W"; cd "$W"
export TMPDIR="$W/tmp"; mkdir -p "$W/tmp"

echo "=== STAGE prep: revlines collection from AGC ($(date -Is))"
if [ ! -f k50_rl.txt ]; then
  $A2F "$AGC" --samples "$SAMPLES" --revlines -o k50_rl.txt 2> prep.log \
    || { echo "PREP FAILED"; tail -5 prep.log; exit 1; }
fi
tail -2 prep.log

echo "=== STAGE grlbwt ($(date -Is))"
if [ ! -f k50_rl.rl_bwt ]; then
  /usr/bin/time -f "grlbwt wall %e s, maxRSS %M KB" \
    $GRL k50_rl.txt -t 64 -T "$TMPDIR" > grl.log 2>&1 \
    || { echo "GRLBWT FAILED"; tail -5 grl.log; exit 1; }
fi
grep -E "Number of runs|BWT size" grl.log | tail -2 || tail -3 grl.log

echo "=== STAGE rle + 5-byte convert"
$RLE k50_rl.rl_bwt k50r
python3 - <<'EOF'
import struct
R = len(open('k50r.syms','rb').read())
vals = struct.unpack(f'<{R}I', open('k50r.len','rb').read()[:R*4])
open('k50rt.bwt.heads','wb').write(open('k50r.syms','rb').read())
with open('k50rt.bwt.len','wb') as f:
    for v in vals: f.write(v.to_bytes(5,'little'))
open('k50rt.scratch','wb').close()
print('k50 rlbwt: R=%d n=%d' % (R, sum(vals)))
EOF

echo "=== STAGE TeraLCP ($(date -Is))"
if [ ! -f k50rt.lcp_index.lcp_index ]; then
  /usr/bin/time -f "teralcp wall %e s, maxRSS %M KB" \
    $TLC -f rlbwt -i k50rt -t k50rt.scratch -oindex k50rt.lcp_index -v quiet 2> tlc.log \
    || { echo "TERALCP FAILED"; tail -5 tlc.log; exit 1; }
fi
ls -la k50rt.lcp_index.lcp_index | awk '{printf "index: %.1f GB\n", $5/1e9}'

echo "=== STAGE chi + samples ($(date -Is))"
if [ ! -f k50.ri4 ]; then
  /usr/bin/time -f "chi wall %e s, maxRSS %M KB" \
    $CHI k50rt.lcp_index.lcp_index --rlbwt k50rt --sidecar k50_rl.txt.names.tsv \
       --samples k50.ri4 -o chi_k50.sA 2> chi.log \
    || { echo "CHI FAILED"; tail -5 chi.log; exit 1; }
fi
tail -3 chi.log

echo "=== STAGE patterns: 200 oracle-planted forward 120-mers"
python3 $TOOLS/pilot_patterns.py --agc "$AGC" --sidecar k50_rl.txt.names.tsv \
  --n 200 --len 120 --seed 42 --out patterns.fa

echo "=== STAGE query ($(date -Is))"
/usr/bin/time -f "query wall %e s, maxRSS %M KB" \
  $RQ k50.ri4 patterns.fa -o k50_occs.txt 2> query.log \
  || { echo "QUERY FAILED"; tail -5 query.log; exit 1; }
tail -2 query.log

echo "=== STAGE verify"
python3 $TOOLS/pilot_verify.py --agc "$AGC" --sidecar k50_rl.txt.names.tsv \
  --patterns patterns.fa --truth patterns.fa.truth.tsv --occs k50_occs.txt 2>&1 | tail -3

echo "=== K50 CHAIN COMPLETE $(date -Is)"
