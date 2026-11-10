#!/usr/bin/env bash
# LANE T1 runner: the tape triad (sorter -> band concat -> finish) over ONE corpus,
# with the LCP-over-runs member (m14) on. This is the SAME three-stage invocation
# `xsa build --tape` issues (xsa/src/chain.rs), spelled out so a lane can run it
# against a hand-built stage directory:
#
#   STAGES=<dir with scatter_sort, cross_lcp_merge, xsa_finish> \
#   CORPUS=<text file> OUT=<dir> [THREADS=24] [MEMORY_GB=24] [KI=8]
#   [SPOOL=1] [FINISH_ARGS="..."] bash tools/binbwt/lcp_runs_run.sh
#
# Every stage log is kept in $OUT. Exits non-zero if any stage does.
set -euo pipefail
: "${STAGES:?set STAGES to the directory holding the three tool binaries}"
: "${CORPUS:?set CORPUS to the text}"
: "${OUT:?set OUT to the run directory}"
THREADS=${THREADS:-24}
MEMORY_GB=${MEMORY_GB:-24}
KI=${KI:-8}
ENVELOPE=${ENVELOPE:-12884901888}
FINISH_ARGS=${FINISH_ARGS:-}
SPOOL=${SPOOL:-0}
mkdir -p "$OUT/merged" "$OUT/bands"
d=$OUT
echo "LCP_RUNS_RUN corpus=$CORPUS bytes=$(stat -c %s "$CORPUS") out=$OUT threads=$THREADS memory_gb=$MEMORY_GB spool=$SPOOL"
"$STAGES/xsa_finish" --corpus "$CORPUS" --sentinel-remap "$d/remap.bin" >"$d/remap.log" 2>&1
if [ "$SPOOL" = 1 ]; then
  : >"$d/checkpoint.journal"
  export XSA_CHECKPOINT_JOURNAL="$d/checkpoint.journal"
  export XSA_FINISH_SPOOL=1
fi
export CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=1100 XSA_TAPE_WAIT_S=inf
unset XSA_KILL_AT XSA_WORK_ROOT || true
"$STAGES/scatter_sort" --corpus "$CORPUS" --remap "$d/remap.bin" --scratch "$d/waves" \
    --tape-bands "$d/bands" --pending-bands "$KI" --budget-bytes "$ENVELOPE" \
    --memory-gb "$MEMORY_GB" --threads "$THREADS" >"$d/order.log" 2>&1 &
order=$!
"$STAGES/cross_lcp_merge" --binbwt "$CORPUS" --remap "$d/remap.bin" \
    --out-prefix "$d/merged/frag" --threads "$THREADS" --tape-bands "$d/bands" \
    >"$d/emit.log" 2>&1 &
emit=$!
# shellcheck disable=SC2086
"$STAGES/xsa_finish" --prefix "$d/merged/frag" --corpus "$CORPUS" --remap "$d/remap.bin" \
    --output "$d/out.sxi2" --threads "$THREADS" --poll-timeout 86400 --with-chi \
    --lcp-bands "$d/bands" $FINISH_ARGS >"$d/finish.log" 2>&1 &
fin=$!
rc=0
wait $fin || rc=1
wait $emit || rc=1
wait $order || rc=1
[ $rc -eq 0 ] || { echo "LCP_RUNS_RUN FAILED (see $d/*.log)"; exit 1; }
echo "LCP_RUNS_RUN_DONE container=$d/out.sxi2 bytes=$(stat -c %s "$d/out.sxi2")"
grep -E "^LCP_RUNS |^LCP_RUNS_ENCODING |^XSA_BUILD_DONE|xsa chi-rspace" "$d/finish.log" || true
