#!/usr/bin/env bash
# prefetch2 harness, part 2: ONE FINISH ARM on a pristine producer output.
#
#   bash tools/binbwt/pf2_arm.sh TAG lazy|scattered|sweep [jobs] [budget_mib] [cold] [bank] [threads]
#
#   lazy        PILE_FINISH_PREFETCH=0 : the demand path's own faults.
#   scattered   PILE_FINISH_PREFETCH=2 : every target window issued in ARRIVAL
#                                        order (batch-scattered).
#   sweep       PILE_FINISH_PREFETCH=1 : sorted, coalesced, one monotone front.
#   jobs        PILE_FINISH_PREFETCH_JOBS   (pipeline depth: bands in flight)
#   budget_mib  PILE_FINISH_PREFETCH_BUDGET (per-job staging budget)
#   cold        1 = drop the whole corpus from the page cache immediately before
#               each arm's finish (PART C), and drop it again inside the finish
#               right before the band sweep.
#
# The producer output is restored from $PRISTINE before every arm, so every arm
# consumes the identical piece set and the finish wall is the only thing that
# differs. The product is compared with the bank by cmp AND sha256; a mismatch is
# a hard failure of the arm, never a footnote.
set -uo pipefail
TAG="${1:?tag}"; ARM="${2:?lazy|scattered|sweep}"; JOBS="${3:-2}"; BUDGET_MIB="${4:-32}"
COLD="${5:-1}"; BANK="${6:-}"; TH="${7:-96}"
COLD_EVERY="${COLD_EVERY:-0}"; ASSIST="${ASSIST:-populate}"; SKIP_RESIDENT="${SKIP_RESIDENT:-1}"
REP="${REP:-1}"
PFB=/mnt/nvme2n1/erikg/pf2-build/pile_finish
COLDTOOL=/mnt/nvme2n1/erikg/pf2-build/cold_cache
ROOT=/mnt/nvme2n1/erikg/pf2-$TAG
PRISTINE="$ROOT/pristine"
CORPUS="${CORPUS:-/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt}"
REMAP=/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin
ARMROOT="$ROOT/arm-$ARM-j$JOBS-b$BUDGET_MIB-c$COLD-e$COLD_EVERY-r$REP"
rm -rf "$ARMROOT"; mkdir -p "$ARMROOT"; rm -rf "$ARMROOT"/frag*
# restore the pristine producer output
# HARDLINKS: the finish only READS these and unlinks its own links, so the
# pristine bytes survive every arm while nothing is copied twice.
cp -al "$PRISTINE"/frag.rlebwt "$PRISTINE"/frag.rlebwt.meta "$PRISTINE"/frag.pieces "$ARMROOT"/
case "$ARM" in
  lazy) MODE=0 ;;
  scattered) MODE=2 ;;
  sweep) MODE=1 ;;
  *) echo "unknown arm $ARM" >&2; exit 2 ;;
esac
# PART C: cold corpus BEFORE the arm (outside the finish, so the claim is about
# the corpus as the finish finds it).
if [ "$COLD" = 1 ] && [ -x "$COLDTOOL" ]; then "$COLDTOOL" "$CORPUS" >> "$ARMROOT/cold.log" 2>&1; fi
echo "=== ARM tag=$TAG arm=$ARM mode=$MODE jobs=$JOBS budget_mib=$BUDGET_MIB cold=$COLD threads=$TH $(date +%T) $(uptime|sed 's/.*load average/load/')"
PILE_FINISH_PREFETCH=$MODE \
PILE_FINISH_PREFETCH_JOBS=$JOBS \
PILE_FINISH_PREFETCH_BUDGET=$((BUDGET_MIB << 20)) \
PILE_FINISH_COLD_CORPUS=$COLD \
PILE_FINISH_COLD_EVERY=$COLD_EVERY \
PILE_FINISH_PREFETCH_ASSIST=$ASSIST \
PILE_FINISH_PREFETCH_SKIP_RESIDENT=$SKIP_RESIDENT \
PILE_FINISH_CACHE_JOURNAL=1 \
  /usr/bin/time -v -o "$ARMROOT/finish.time" "$PFB" --prefix "$ARMROOT/frag" \
    --corpus "$CORPUS" --remap "$REMAP" --output "$ARMROOT/product.sxi2" \
    --threads "$TH" --poll-timeout 3600 --wit-ram-budget 1073741824 \
    > /dev/null 2> "$ARMROOT/finish.log"
FRC=$?
FIN=$(grep -oE "sweep wall=[0-9.]+" "$ARMROOT/finish.log" | head -1 | cut -d= -f2)
ELAPSED=$(grep -oE "Elapsed \(wall clock\).*" "$ARMROOT/finish.time" | head -1)
SHA=$(sha256sum "$ARMROOT/product.sxi2" 2>/dev/null | cut -d' ' -f1)
CMP=na; [ -n "$BANK" ] && { cmp -s "$ARMROOT/product.sxi2" "$BANK" && CMP=0 || CMP=1; }
CHI=$(grep -oE "PILE_FINISH_DONE chi=[0-9]+" "$ARMROOT/finish.log" | head -1 | cut -d= -f2)
PFC=$(grep -oE "PILE_FINISH_PREFETCH jobs=.*" "$ARMROOT/finish.log" | tail -1)
RESID=$(grep -oE "PILE_FINISH_CACHE_RESIDENCY label=finish.*" "$ARMROOT/finish.log" | tail -1)
COLDSUM=$(grep -oE "PILE_FINISH_COLD_CORPUS .*" "$ARMROOT/finish.log" | tail -1)
LINE="REP=$REP TAG=$TAG ARM=$ARM MODE=$MODE JOBS=$JOBS BUDGET_MIB=$BUDGET_MIB COLD=$COLD COLD_EVERY=$COLD_EVERY ASSIST=$ASSIST RC=$FRC SWEEP_WALL=$FIN CHI=$CHI CMP=$CMP SHA=$SHA $ELAPSED"
printf '%s\n' "$LINE" | tee -a "$ROOT/../pf2-results.tsv"
{
  echo "$LINE"
  echo "  $PFC"
  echo "  $COLDSUM"
  echo "  $RESID"
  grep -oE "PILE_FINISH_CACHE_RESIDENCY label=[a-z0-9-]+ .*resident_pct=[0-9.]+" "$ARMROOT/finish.log" | sed 's/^/  /'
  grep -E "PILE_FINISH_REGION (lce-span|advance-witness|readband)|SLIM_FP pile-finish-corpus" "$ARMROOT/finish.log" | sed 's/^/  /'
  echo "  $ELAPSED"
} >> "$ROOT/../pf2-results-detail.log"
echo "=== ARM_DONE tag=$TAG arm=$ARM $(date +%T)"
