#!/usr/bin/env bash
# PART C, measurement 1: the 1 GB cold-corpus THREE-WAY.
#
#   lazy      | batch-scattered      | batch-sweep
#   demand    | every target window  | sorted, coalesced, monotone front,
#   faults    | in arrival order     | skipping the pages that are resident
#
# Every arm runs the SAME pristine producer output (one producer run, copied per
# arm), the corpus is dropped from the page cache immediately before each arm and
# again inside the finish right before the band sweep, and the arms are
# INTERLEAVED (rep, then arm) so a drifting box load hits all three equally. The
# box is shared: each line carries the load average it ran under, and the result
# is read as a ratio, not as an absolute second.
#
#   bash tools/binbwt/pf2_threeway.sh TAG REPS COLD_EVERY [BANK]
set -uo pipefail
TAG="${1:?tag}"; REPS="${2:-2}"; COLD_EVERY="${3:-0}"
BANK="${4:-/mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
for rep in $(seq 1 "$REPS"); do
  for arm in lazy scattered sweep; do
    REP="$rep" COLD_EVERY="$COLD_EVERY" bash "$HERE/pf2_arm.sh" "$TAG" "$arm" 2 32 1 "$BANK" 96
  done
done
echo "=== THREEWAY_DONE tag=$TAG reps=$REPS cold_every=$COLD_EVERY $(date +%T)"
