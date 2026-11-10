#!/usr/bin/env bash
# G4 -- 100 GB (/mnt/nvme2n1/erikg/pile-head100.txt, exactly 100,000,000,000 B)
#       with --scratch-budget-gb 30, on the DEFAULT arm (no m14).
#
# The default arm is the one that costs you nothing: it writes the v7 layout, so the
# container is the pre-m14 container byte for byte. What this gate proves at scale:
#
#   * the tape triad COMPLETES at 100 GB inside a 30 GB scratch budget
#   * container size (the cost of NOT having m14), chi, per-phase wall, peak scratch
#   * the container declares its own provenance: version 7, no member m14, header
#     flag bit 7 clear, and the plan line says lcp_runs=absent
#   * `xsa lcp` on it is SKIPPED BY ABSENCE - it refuses by name, it does not guess
#   * the reader still answers: `--count " function "` = 4,785,152, the value three
#     independent implementations settled on (RESULTS_PILE_100GB.md)
#
# The m14-ON arm at scale is reported separately (gates/lcp_runs_g3_10g.sh: 10 GB
# container 9,560,930,192 B, m14 2,556,383,072 B, LCP_VERIFY 140/140 exact).
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
fail=0

echo "G4_CORPUS $(stat -c %s "$CORPUS") bytes  threads=$THREADS memory_gb=$MEM finish_as_gb=$FIN scratch_budget_gb=$BUDGET arm=default(no-m14)"
echo "G4_BOX load=$(cut -d' ' -f1-3 /proc/loadavg) mem_available_gb=$(awk '/MemAvailable/{printf "%.0f", $2/1048576}' /proc/meminfo) free_disk_gb=$(df --output=avail -BG /mnt/nvme2n1 | tail -1 | tr -dc 0-9)"

if [ "${SKIP_BUILD:-0}" != 1 ]; then
  rm -rf "$D/scratch"
  /usr/bin/time -f "G4_WALL %e s  RSS %M kB" setsid timeout --kill-after=10 129600 \
    "$X" build --tape --input "$CORPUS" --scratch "$D/scratch" \
    --threads "$THREADS" --memory-gb "$MEM" --finish-as-gb "$FIN" \
    --scratch-budget-gb "$BUDGET" >"$D/build.log" 2>&1
  echo "G4_BUILD_RC=$?"
fi
grep -E "^(XSA_PLAN_TAPE |XSA_PLAN_DISC |SCRATCH_BUDGET|G4_WALL|XSA_BUILD_DONE)" "$D/build.log" | cut -c1-600
grep -E "^(PHASE |LCP_RUNS_POLICY|SCRATCH_BACKPRESSURE_SUMMARY|SXI2_MEMBER|CONTAINER_FINAL)" \
    "$D/scratch/finish-v2.log" "$D/scratch/order-tape.log" 2>/dev/null

grep -q "XSA_BUILD_DONE chi=$CHI_EXPECT " "$D/build.log" \
  && echo "G4_CHI chi=$CHI_EXPECT equals_the_pre_m14_build=1" \
  || { echo "G4_CHI_MISMATCH"; grep XSA_BUILD_DONE "$D/build.log"; fail=1; }
grep -q "lcp_runs=absent" "$D/scratch/xsa-build.log" 2>/dev/null \
  || grep -q "lcp_runs=absent" "$D/build.log" \
  && echo "G4_PLAN lcp_runs=absent member=m14 declared=no" || { echo "FAIL: plan line does not say absent"; fail=1; }

BYTES=$(stat -c %s "$C"); N=100000000001
echo "G4_CONTAINER_BYTES $BYTES = $(awk -v b="$BYTES" -v n="$N" 'BEGIN{printf "%.6f", b/n}') n"
echo "G4_SHA256 $(sha256sum "$C" | cut -d' ' -f1)"
# the container's OWN provenance: version 7, 8 members, no m14, flag bit 7 clear
read -r VER CNT FLAGS LEN FSIZE < <(python3 - "$C" <<'PY'
import struct,sys,os
p=sys.argv[1]; h=open(p,'rb').read(64)
print(struct.unpack_from('<I',h,4)[0], struct.unpack_from('<I',h,32)[0],
      struct.unpack_from('<Q',h,48)[0], struct.unpack_from('<Q',h,40)[0], os.path.getsize(p))
PY
)
echo "G4_HEADER version=$VER members=$CNT flags=$FLAGS flag128=$(( (FLAGS>>7)&1 )) length_matches=$([ "$LEN" = "$FSIZE" ] && echo 1 || echo 0)"
[ "$VER" = 7 ] && [ "$CNT" = 8 ] && [ $(( (FLAGS>>7)&1 )) = 0 ] \
  || { echo "FAIL: the default arm did not write the v7 layout"; fail=1; }

# LCP_VERIFY on this container is SKIPPED BY ABSENCE, and the skip is a refusal.
"$X" lcp "$C" --run 3 >"$D/lcp_absent.out" 2>"$D/lcp_absent.log"
rc=$?
head -3 "$D/lcp_absent.log"
[ $rc != 0 ] && grep -q "has no member m14" "$D/lcp_absent.log" \
  && echo "G4_LCP_VERIFY skipped=absence rc=$rc by_name=1" \
  || { echo "FAIL: a 100 GB default container did not refuse m14 by name (rc=$rc)"; fail=1; }

# the reader still answers from the container
timeout 3600 "$X" query "$C" --count " function " >"$D/count.out" 2>"$D/count.log"
grep -E "QLITE_OPEN|QUERY_DONE" "$D/count.log" | cut -c1-400
# --count tsv is "pattern_id<TAB>pattern<TAB>occurrences"; there is one pattern here.
GOT=$(awk -F'\t' '{print $NF}' "$D/count.out" | head -1)
[ "${GOT:-0}" = "$COUNT_EXPECT" ] && echo "G4_COUNT function=$GOT matches_the_three_way_verified_value=1" \
  || { echo "G4_COUNT_MISMATCH got=${GOT:-none} expected=$COUNT_EXPECT"; fail=1; }

echo "SURVIVORS: $(pgrep -a -f 'xsa_finish|scatter_sort|cross_lcp_merge' | grep -v pgrep || echo none)"
if [ $fail = 0 ]; then echo "GATE G4 PASS"; else echo "GATE G4 FAIL"; exit 1; fi
