#!/usr/bin/env bash
# G4 -- 100 GB (/mnt/nvme2n1/erikg/pile-head100.txt, exactly 100,000,000,000 B)
#       with --scratch-budget-gb 30.
#
#   * the tape triad COMPLETES at scale with m14 on
#   * container size, member bytes, ratio, chi, per-phase wall, peak scratch
#   * m14's structure audits at Pile scale (30.6 M blocks: the directory tiles
#     22 GB of payload exactly and every escape list is sorted)
#   * the reader still answers: `--count " function "` must be 4,785,152, the
#     value three independent implementations settled on (RESULTS_PILE_100GB.md)
#
# usage: gates/lcp_runs_g4_100g.sh [threads] [memory_gb] [finish_as_gb] [scratch_budget_gb]
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pile-head100.txt}
D=${D:-/mnt/nvme2n1/erikg/lcpruns-gate/g4}
THREADS=${1:-96}
MEM=${2:-96}
FIN=${3:-400}
BUDGET=${4:-30}
CHI_EXPECT=${CHI_EXPECT:-24185339204}
COUNT_EXPECT=${COUNT_EXPECT:-4785152}
mkdir -p "$D"
C=$D/scratch/pile-head100.sxi2

echo "G4_CORPUS $(stat -c %s "$CORPUS") bytes  threads=$THREADS memory_gb=$MEM finish_as_gb=$FIN scratch_budget_gb=$BUDGET"
echo "G4_BOX load=$(cut -d' ' -f1-3 /proc/loadavg) mem_available_gb=$(awk '/MemAvailable/{printf "%.0f", $2/1048576}' /proc/meminfo) free_disk_gb=$(df --output=avail -BG /mnt/nvme2n1 | tail -1 | tr -dc 0-9)"

if [ "${SKIP_BUILD:-0}" != 1 ]; then
  rm -rf "$D/scratch"
  /usr/bin/time -f "G4_WALL %e s  RSS %M kB" setsid timeout --kill-after=10 129600 \
    "$X" build --tape --input "$CORPUS" --scratch "$D/scratch" \
    --threads "$THREADS" --memory-gb "$MEM" --finish-as-gb "$FIN" \
    --scratch-budget-gb "$BUDGET" >"$D/build.log" 2>&1
  echo "G4_BUILD_RC=$?"
fi
fail=0
grep -E "^(G4_WALL|XSA_BUILD_DONE|XSA_PLAN_RAM|XSA_PLAN_DISC |SCRATCH_BUDGET)" "$D/build.log" | head -8
grep -E "^(PHASE |LCP_RUNS|LCP_RUNS_ENCODING|LCP_RUNS_DEPTH|CONTAINER_FINAL|SCRATCH_BACKPRESSURE_SUMMARY)" \
    "$D/scratch/finish-v2.log" "$D/scratch/order-tape.log" 2>/dev/null
grep -hE "^LCP_RUNS |^LCP_RUNS_ENCODING|^LCP_RUNS_DEPTH|^CONTAINER_FINAL" "$D/scratch/finish-v2.log"

grep -q "XSA_BUILD_DONE chi=$CHI_EXPECT " "$D/build.log" \
  && echo "G4_CHI chi=$CHI_EXPECT matches_the_banked_100GB_build=1" \
  || { echo "G4_CHI_MISMATCH"; grep XSA_BUILD_DONE "$D/build.log"; fail=1; }
echo "G4_CONTAINER_BYTES $(stat -c %s "$C")"
echo "G4_SHA256 $(sha256sum "$C" | cut -d' ' -f1)"
MB=$(grep -hoE "^LCP_RUNS runs=[0-9]+ bytes=[0-9]+" "$D/scratch/finish-v2.log" | grep -oE "bytes=[0-9]+" | tr -dc 0-9)
[ "${MB:-0}" -gt 0 ] && echo "G4_MEMBER_BYTES bytes=$MB" || { echo "FAIL: no m14 bytes"; fail=1; }

# ---- the member audits at Pile scale (a full directory walk + every escape list)
/usr/bin/time -f "G4_AUDIT_WALL %e s RSS %M kB" timeout 3600 \
  "$X" lcp "$C" --audit --run 0 --sample 3 --seed 5 >"$D/lcp_probe.tsv" 2>"$D/lcp_probe.log"
rc=$?
grep -E "^LCP_OPEN|^LCP_AUDIT|^LCP_ACCESS|^G4_AUDIT_WALL" "$D/lcp_probe.log"
[ $rc = 0 ] || { echo "FAIL: xsa lcp exited $rc"; fail=1; }
grep -q "tiling=exact escape_lists=sorted" "$D/lcp_probe.log" || { echo "FAIL: m14 audit at scale"; fail=1; }

# ---- the reader still answers from the new (v8) container
timeout 3600 "$X" query "$C" --count " function " >"$D/count.out" 2>"$D/count.log"
grep -E "QLITE_OPEN|QUERY_DONE" "$D/count.log" | head -3
# --count tsv is "pattern_id<TAB>occurrences"; there is exactly one pattern here.
GOT=$(awk -F'\t' '$1==0{print $2}' "$D/count.out")
[ "${GOT:-0}" = "$COUNT_EXPECT" ] && echo "G4_COUNT function=$GOT matches_three_way_verified=1" \
  || { echo "G4_COUNT_MISMATCH got=${GOT:-none} expected=$COUNT_EXPECT"; fail=1; }

echo "SURVIVORS: $(pgrep -a -f 'xsa_finish|scatter_sort|cross_lcp_merge' | grep -v pgrep || echo none)"
if [ $fail = 0 ]; then echo "GATE G4 PASS"; else echo "GATE G4 FAIL"; exit 1; fi
