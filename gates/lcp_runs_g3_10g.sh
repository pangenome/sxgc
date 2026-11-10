#!/usr/bin/env bash
# G3 -- 10 GB (/mnt/nvme2n1/erikg/pf-10gb.txt, 10,000,000,000 B)
#
#   * the tape triad completes; chi = 2,676,925,006 (the banked value, unchanged)
#   * member m14 is present (SXI2 v8, flag bit 7) with its bytes and ratio
#   * the m14 structural audit passes at scale (directory tiles the payload)
#   * one random-access answer is readable and cheap
#   * `xsa query` still answers from the same container
#
# usage: gates/lcp_runs_g3_10g.sh [threads] [memory_gb] [finish_as_gb]
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pf-10gb.txt}
D=${D:-/mnt/nvme2n1/erikg/lcpruns-gate/g3}
THREADS=${1:-48}
MEM=${2:-96}
FIN=${3:-200}
CHI_EXPECT=${CHI_EXPECT:-2676925006}
mkdir -p "$D"
C=$D/scratch/pf-10gb.sxi2

if [ "${SKIP_BUILD:-0}" != 1 ]; then
  rm -rf "$D/scratch"
  /usr/bin/time -f "G3_WALL %e s  RSS %M kB" setsid timeout --kill-after=10 21600 \
    "$X" build --tape --input "$CORPUS" --scratch "$D/scratch" \
    --threads "$THREADS" --memory-gb "$MEM" --finish-as-gb "$FIN" >"$D/build.log" 2>&1
  echo "G3_BUILD_RC=$?"
fi
grep -E "^(G3_WALL|XSA_BUILD_DONE|LCP_RUNS|LCP_RUNS_ENCODING|LCP_RUNS_DEPTH|CONTAINER_FINAL|SCRATCH_BACKPRESSURE_SUMMARY|PHASE )" "$D/build.log"
grep -hE "^LCP_RUNS |^LCP_RUNS_ENCODING|^LCP_RUNS_DEPTH|^CONTAINER_FINAL" "$D/scratch/finish-v2.log"

fail=0
grep -q "XSA_BUILD_DONE chi=$CHI_EXPECT " "$D/build.log" \
  && echo "G3_CHI chi=$CHI_EXPECT unchanged=1" \
  || { echo "G3_CHI_MISMATCH"; grep XSA_BUILD_DONE "$D/build.log"; fail=1; }
MB=$(grep -hoE "^LCP_RUNS runs=[0-9]+ bytes=[0-9]+" "$D/scratch/finish-v2.log" | grep -oE "bytes=[0-9]+" | tr -dc 0-9)
[ "${MB:-0}" -gt 0 ] && echo "G3_MEMBER_BYTES bytes=$MB" || { echo "FAIL: no m14 bytes"; fail=1; }

"$X" lcp "$C" --audit --run 0 --sample 4 --seed 5 >"$D/lcp_probe.tsv" 2>"$D/lcp_probe.log"
rc=$?
grep -E "^LCP_OPEN|^LCP_AUDIT|^LCP_ACCESS" "$D/lcp_probe.log"
[ $rc = 0 ] || { echo "FAIL: xsa lcp exited $rc"; fail=1; }
grep -q "tiling=exact escape_lists=sorted" "$D/lcp_probe.log" || { echo "FAIL: m14 audit"; fail=1; }

# ---- THE m14-ON ARM AT SCALE: 50+ stored values against brute force.
timeout 3600 "$X" lcp-verify "$C" --corpus "$CORPUS" --sample 50 --deep 50 \
    --deep-windows 64 --seed 11 >"$D/verify.log" 2>&1
rc=$?
grep -E "^LCP_VERIFY_DEEP |^LCP_VERIFY n=|^LCP_MISMATCH" "$D/verify.log"
[ $rc = 0 ] || { echo "FAIL: lcp-verify exited $rc"; tail -3 "$D/verify.log"; fail=1; }
VN=$(grep -oE "^LCP_VERIFY n=[0-9]+" "$D/verify.log" | tr -dc 0-9)
VM=$(grep -oE "mismatches=[0-9]+" "$D/verify.log" | tr -dc 0-9)
[ "${VN:-0}" -ge 50 ] || { echo "FAIL: only ${VN:-0} runs verified"; fail=1; }
[ "${VM:-1}" = 0 ] || { echo "FAIL: $VM mismatches"; fail=1; }

timeout 900 "$X" query "$C" --limit 3 " function " >"$D/query.out" 2>"$D/query.log"
grep -E "QLITE_OPEN|QUERY_DONE" "$D/query.log"
grep -q "version=8" "$D/query.log" || { echo "FAIL: reader did not open v8"; fail=1; }
echo "G3_QUERY_OCCURRENCES $(grep -oE 'occurrences=[0-9]+' "$D/query.log")"

echo "SURVIVORS: $(pgrep -a -f 'xsa_finish|scatter_sort|cross_lcp_merge' | grep -v pgrep || echo none)"
if [ $fail = 0 ]; then echo "GATE G3 PASS"; else echo "GATE G3 FAIL"; exit 1; fi
