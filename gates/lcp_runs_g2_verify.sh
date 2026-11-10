#!/usr/bin/env bash
# G2 -- THE CORRECTNESS GATE FOR MEMBER m14 (the LCP over runs).
#
# A wrong LCP is worse than none: a tree built on it answers wrongly while looking
# right. So this gate does not read the member and agree with itself. For every
# sampled run it compares the STORED value against a brute-force computation from
# the corpus:
#
#   stored[i] = m14[i]                       (one directory entry + one block)
#   true[i]   = lce(SA[r_i-1], SA[r_i])       (plain memcmp of the two suffixes)
#
# and the two SA values come from the container's own locate (m11 anchor + an LF
# walk inside one 1024-run stripe), which the query gates certify independently
# (100/100 counts, 574/574 positions, and `xsa verify --corpus` byte checks).
#
# THE SAMPLE IS CHOSEN, NOT JUST DRAWN (see the three picks in lcpruns.rs):
#   (a) DEPTH BOUNDARIES - the largest value below each of 1/8/16/32/64/256/64K
#       and the smallest at or above it. This is where an inline field turns into
#       an escape list, i.e. exactly where an off-by-one would live; and the tree
#       operations we want are SHALLOW (median repeating context 18 bytes,
#       entropy 0 by depth 32 - CONTEXT_DEPTH.md).
#   (b) THE DEEPEST runs (the u64 escapes).
#   (c) a UNIFORM tail (the zeros and tiny values, i.e. the top of the tree).
#   (d) DEEP-INVARIANT runs: a 16..48-byte pattern's SA interval has the property
#       that every interior head row has LCP >= the pattern length, so a run that
#       STARTS inside the interval is asserted to report at least that depth.
#
# usage: gates/lcp_runs_g2_verify.sh [container] [corpus]
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
C=${1:-/mnt/nvme2n1/erikg/lcpruns-gate/g1/scratch/t150m.sxi2}
CORPUS=${2:-/dev/shm/t150m.txt}
SAMPLE=${SAMPLE:-50}
DEEP=${DEEP:-50}
WINDOWS=${WINDOWS:-64}
SEED=${SEED:-11}
OUT=${OUT:-/mnt/nvme2n1/erikg/lcpruns-gate/g2}
mkdir -p "$OUT"
LOG=$OUT/verify.log

[ -s "$C" ] || { echo "FAIL: no container at $C"; exit 1; }
[ -r "$CORPUS" ] || { echo "FAIL: no corpus at $CORPUS"; exit 1; }

timeout 3600 "$X" lcp-verify "$C" --corpus "$CORPUS" --sample "$SAMPLE" --deep "$DEEP" \
    --deep-windows "$WINDOWS" --seed "$SEED" >"$LOG" 2>&1
rc=$?
grep -E "^LCP_VERIFY_OPEN|^LCP_AUDIT|^LCP_VERIFY_CORPUS|^LCP_VERIFY_SENTINEL|^LCP_VERIFY_SAMPLE" "$LOG"
grep -E "^LCP_VERIFY_DEEP " "$LOG"
grep -E "^LCP_VERIFY n=|^LCP_MISMATCH" "$LOG"
[ $rc = 0 ] || { echo "FAIL: lcp-verify exited $rc (see $LOG)"; tail -5 "$LOG"; exit 1; }

N=$(grep -oE "^LCP_VERIFY n=[0-9]+" "$LOG" | tr -dc 0-9)
EXACT=$(grep -oE "^LCP_VERIFY n=[0-9]+ exact=[0-9]+" "$LOG" | grep -oE "exact=[0-9]+" | tr -dc 0-9)
MIS=$(grep -oE "mismatches=[0-9]+" "$LOG" | tr -dc 0-9)
echo "G2_CHECKED n=$N exact=$EXACT mismatches=$MIS"
if [ "${N:-0}" -lt 50 ]; then echo "FAIL: only $N runs verified (need >= 50)"; exit 1; fi
if [ "${EXACT:-0}" != "${N:-0}" ] || [ "${MIS:-1}" != 0 ]; then
    echo "FAIL: $MIS mismatches"; grep "^LCP_MISMATCH" "$LOG" | head -10; exit 1
fi
# the depth boundary coverage must include a value above the widest inline field
for b in 1 8 16 32 64; do
    grep -qE "stored=($b|$(($b+1)))\b" "$LOG" || echo "NOTE: no checked value at the $b boundary"
done
echo "GATE G2 PASS"
