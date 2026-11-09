#!/bin/bash
# ONE GATED RUN, with a wall-clock guard and a survivor check.
#
# The PRODUCT has no timeout (xsa build never kills a phase on a guess). This
# harness is the OUTSIDE guard a human uses around one gate: setsid puts the
# build in its own session so `timeout --kill-after=10` can take the whole tree
# down if the box or the gate misbehaves, and the gate then ASSERTS that no
# stage process survived (a leaked scatter_sort/xsa_finish would poison the next
# gate's measurement and hold the scratch directory).
#
#   CAP_TIMEOUT=<seconds> cap_gate.sh <log> <scratch> -- <xsa build argv...>
#
# Env: CAP_TIMEOUT (guard, default 6 h), XSA (binary, default the worktree build)
set -u
XSA="${XSA:-/mnt/nvme2n1/erikg/sxgc-cap-catalogue/xsa/target/release/xsa}"
CAP_TIMEOUT="${CAP_TIMEOUT:-21600}"
LOG="${1:?log path}"; shift
SCRATCH="${1:?scratch dir}"; shift
[ "${1:-}" = "--" ] && shift

mkdir -p "$SCRATCH"
START=$(date +%s)
setsid timeout --kill-after=10 "$CAP_TIMEOUT" "$XSA" "$@" >"$LOG" 2>&1
RC=$?
END=$(date +%s)
WALL=$((END-START))

SURVIVORS=$( { pgrep -x scatter_sort; pgrep -x cross_lcp_merge; pgrep -x xsa_finish; } | tr '\n' ' ')
if [ -n "${SURVIVORS// /}" ]; then
  echo "GATE_FAIL rc=$RC wall=${WALL}s survivors=$SURVIVORS (the guard left processes behind)"
  exit 97
fi
if [ "$RC" -ne 0 ]; then
  echo "GATE_FAIL rc=$RC wall=${WALL}s survivors=none (see $LOG)"
  exit "$RC"
fi
echo "GATE_OK wall=${WALL}s survivors=none log=$LOG"
