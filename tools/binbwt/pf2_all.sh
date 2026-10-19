#!/usr/bin/env bash
# The whole part-2 measurement campaign, seeded from ONE pristine 1 GB producer
# output. Detached, sequential (my own arms must not compete with each other on a
# box that already carries the operator's live 10 GB A/B):
#
#   A. the three-way with a COLD START only (cold_every=0): the corpus is dropped
#      once, right before the sweep. This is the operator's "cold-corpus arm".
#   B. the three-way with a CONTINUOUSLY cold corpus (cold_every=64): the text is
#      dropped again every 64 bands, i.e. the pool is ~1/3 of the text and can
#      never fully warm - the pile regime at fixture scale.
#   C. the balance curve (depth x staging) on the continuously-cold model.
#
#   bash tools/binbwt/pf2_all.sh TAG [REPS]
set -uo pipefail
TAG="${1:?tag}"; REPS="${2:-2}"
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "=== CAMPAIGN start tag=$TAG reps=$REPS $(date +%T) $(uptime|sed 's/.*load average/load/')"
bash "$HERE/pf2_threeway.sh" "$TAG" "$REPS" 0
bash "$HERE/pf2_threeway.sh" "$TAG" "$REPS" 64
bash "$HERE/pf2_balance.sh"  "$TAG" 64 "1 2 4 8" "8 32 128" 1
echo "=== CAMPAIGN_DONE tag=$TAG $(date +%T)"
