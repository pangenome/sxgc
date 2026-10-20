#!/usr/bin/env bash
# PART C, measurement 2: THE BALANCE CURVE at 1 GB, cold corpus.
#
# Two knobs, one grid:
#   PIPELINE DEPTH  PILE_FINISH_PREFETCH_JOBS   = bands of targets in flight at
#                   once (the density of merged in-flight tasks);
#   STAGING SIZE    PILE_FINISH_PREFETCH_BUDGET = bytes of merged front one job
#                   may stage (the buffer budget).
#
# The point of the curve is that they TRADE: depth x budget is the read-ahead's
# RAM footprint, and the finish wall is a function of both, not of either alone.
#
#   bash tools/binbwt/pf2_balance.sh TAG COLD_EVERY [JOBS_LIST] [BUDGET_LIST] [REPS]
set -uo pipefail
TAG="${1:?tag}"; COLD_EVERY="${2:-0}"
JOBS_LIST="${3:-1 2 4 8}"; BUDGET_LIST="${4:-8 32 128}"; REPS="${5:-1}"
BANK="${BANK:-/mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
for rep in $(seq 1 "$REPS"); do
  for j in $JOBS_LIST; do
    for b in $BUDGET_LIST; do
      echo "# balance rep=$rep jobs=$j budget=$b"; REP="$rep" COLD_EVERY="$COLD_EVERY" bash "$HERE/pf2_arm.sh" "$TAG" sweep "$j" "$b" 1 "$BANK" 96
    done
  done
done
echo "=== BALANCE_DONE tag=$TAG jobs=[$JOBS_LIST] budgets=[$BUDGET_LIST] cold_every=$COLD_EVERY $(date +%T)"
