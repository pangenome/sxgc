#!/usr/bin/env bash
# Fixed 100 MB, four fit-only whole-wave epochs, half-N posting cap. No finish or scale override.
set -euo pipefail
source "$(dirname "$0")/overlap_finite_treatment.sh";finite_treatment epochs "$@"
