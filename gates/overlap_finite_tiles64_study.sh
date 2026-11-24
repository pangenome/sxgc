#!/usr/bin/env bash
# Fixed expansion-only arm; do not rerun a banked emitter.
set -euo pipefail
source "$(dirname "$0")/overlap_finite_treatment.sh";finite_treatment tiled "$@"
