#!/usr/bin/env bash
# Fixed post-gather finite sort treatment vs the completed tiled arm.
set -euo pipefail
source "$(dirname "$0")/overlap_finite_treatment.sh";finite_treatment cached "$@"
