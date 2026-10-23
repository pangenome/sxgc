#!/usr/bin/env bash
# kasai_finish_gate.sh - the whole kasai-finish gate suite, in order, on one box.
#
#   bash tools/binbwt/kasai_finish_gate.sh 100m|1b BUILD_DIR ROOT
#
#   0. the repo's existing oracle suites (the NRun/rank change must not break them)
#   1. the tiny brute-force oracle + its NEGATIVE arm (exit 2 required)
#   2. produce a pristine producer output WITH the k=1 SA side-stream
#   3. build the column (+ the locate table) and prove the two inversion arms
#      produce byte-identical ISA and column files
#   4. the three finish arms on the identical piece set: old / kasai / audit
#   5. cmp every product against the bank and check ON == OFF
#
# Every wall is labelled with the box's load average: this box carries the
# operator's work and none of these numbers is a quiet-box number.
set -uo pipefail
cd "$(dirname "$0")/../.."
SIZE="${1:?usage: kasai_finish_gate.sh 100m|1b BUILD_DIR ROOT}"
BUILD="$(cd "${2:?}" && pwd)"
ROOT="${3:?}"; mkdir -p "$ROOT"
BASE=/mnt/nvme2n1/erikg
REMAP=$BASE/sol6c-gate100m/remap-256.bin
case "$SIZE" in
  100m) FIX=$BASE/extcols-scratch/mergperf-100m-snap.txt
        BANK=$BASE/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2
        CHI=29716349; NCAP=$(stat -c%s "$FIX"); ULIMIT=8388608 ;;
  1b)   FIX=$BASE/extcols-scratch/mergperf-1b-snap.txt
        BANK=$BASE/extcols-scratch/pilev2-g2-1b/frag.sxi2
        CHI=283296933; NCAP=$(stat -c%s "$FIX"); ULIMIT=41943040 ;;
  *) echo "unknown size $SIZE" >&2; exit 2 ;;
esac
TH=${THREADS:-96}
say() { echo "### $* (loadavg $(cut -d' ' -f1 /proc/loadavg) $(date +%T))"; }

say "0. existing oracle suites"
"$BUILD/pile_finish" --selftest-repair-chunks 2>/dev/null | tail -1
"$BUILD/pile_finish" --selftest-sweep-monoid 2>/dev/null | tail -1
mkdir -p "$ROOT/seamspool"; "$BUILD/pile_finish" --selftest-seam-spool "$ROOT/seamspool" 2>/dev/null | tail -1

say "1. the tiny oracle"
KASAI_SELFTEST_DIR="$ROOT" "$BUILD/kasai_column" --selftest 2>/dev/null
KASAI_SELFTEST_DIR="$ROOT" "$BUILD/kasai_column" --selftest-negative 2>&1 | tail -2
NEG=$?
echo "NEGATIVE_ARM exit=$NEG (must be 2)"

say "2. produce (k=1 SA side-stream, file mode)"
D="$ROOT/prod"; rm -rf "$D"; mkdir -p "$D"
( ulimit -v "$ULIMIT"
  CROSS_POOL_SLOTS=33554432 CROSS_V2_PIECE_WINDOW=1000000 PHASE_CAP_KB=8388608 \
  /usr/bin/time -f "PRODUCER_WALL=%e" "$BUILD/binbwt" --binbwt "$FIX" --remap "$REMAP" \
    --out-prefix "$D/frag" --threads "$TH" --bin-live-cap "$NCAP" \
    --sa-sample-k 1 --sa-sample-out "$D/anchors.sxsa" > "$D/bin.log" 2>&1 )
grep -E "SA_ANCHOR_DONE|PRODUCER_WALL" "$D/bin.log"
mkdir -p "$D/pristine"
cp -al "$D/frag.pieces" "$D/pristine/frag.pieces"
cp -al "$D/frag.rlebwt" "$D/pristine/frag.rlebwt"
cp -al "$D/frag.rlebwt.meta" "$D/pristine/frag.rlebwt.meta" 2>/dev/null

say "3. the column (+ locate table), both inversion arms"
"$BUILD/kasai_column" --corpus "$FIX" --remap "$REMAP" --sa "$D/anchors.sxsa" \
  --out "$D/col.sxlc" --threads "$TH" --invert direct --ram-budget 8589934592 \
  --keep-isa 2>&1 | tail -2
"$BUILD/kasai_column" --corpus "$FIX" --remap "$REMAP" --sa "$D/anchors.sxsa" \
  --out "$D/colb.sxlc" --threads "$TH" --invert bucketed --ram-budget 67108864 \
  --buckets 64 --keep-isa 2>&1 | tail -2
cmp "$D/col.sxlc.isa" "$D/colb.sxlc.isa" && echo ISA_IDENTICAL_direct_vs_bucketed64
cmp "$D/col.sxlc" "$D/colb.sxlc" && echo COLUMN_IDENTICAL_direct_vs_bucketed64

say "4. the three finish arms"
export PRISTINE="$D/pristine" CORPUS="$FIX" REMAP BANK PF="$BUILD/pile_finish"
for arm in old kasai audit; do
  bash tools/binbwt/kasai_finish_arm.sh "$ROOT" "$SIZE" "$arm" "$D/col.sxlc" "$TH" "$CHI" | grep -E "TAG=|ARM_DONE"
done

say "5. ON == OFF and the bank"
cmp "$ROOT/old/product.sxi2" "$ROOT/kasai/product.sxi2" && echo ON_EQ_OFF_BYTES
for arm in old kasai audit; do
  cmp -s "$ROOT/$arm/product.sxi2" "$BANK" && echo "BANK_OK $arm" || echo "BANK_FAIL $arm"
done
say "GATE_DONE $SIZE"
