#!/usr/bin/env bash
# The 10 GB pile-regime hygiene A/B, both arms, sequential, detached.
#   bash tools/binbwt/pf2_hygiene_ab.sh [threads]
# hyg0 (hygiene OFF, the control) runs first, then hyg1 (hygiene ON), so the
# logs are complete per arm even if the box's other jobs change underneath.
set -uo pipefail
TH="${1:-96}"
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "=== HYGIENE_AB_ALL start $(date +%T) $(uptime|sed 's/.*load average/load/')"
bash "$HERE/pf2_hygiene10g.sh" hyg0 "$TH"
bash "$HERE/pf2_hygiene10g.sh" hyg1 "$TH"
echo "=== HYGIENE_AB_ALL_DONE $(date +%T)"
