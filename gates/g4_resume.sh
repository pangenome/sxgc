#!/usr/bin/env bash
# G4: kill the FINISH mid-run, restart it, and require the container to be
# byte-identical to the uninterrupted build, with a real container-offset resume.
#
# The sorter and the emitter stay alive across the finish's death - that is the
# native resume shape the finish's own checkpoint protocol defines (the producer
# is not restarted here; RESUME_DESIGN.md's Lane A is a separate, larger change).
#
# Usage: g4_resume.sh STAGE_DIR CORPUS WORK_DIR
#   STAGE_DIR holds scatter_sort, cross_lcp_merge and xsa_finish.
set -euo pipefail
STAGES=$(cd "${1:?stage dir}" && pwd)
CORPUS=$(cd "$(dirname "${2:?corpus}")" && pwd)/$(basename "$2")
ROOT=${3:?work dir}
THREADS=${G4_THREADS:-24}
mkdir -p "$ROOT"

assert_no_survivors() {
  local pids
  pids=$(pgrep -x scatter_sort -x cross_lcp_merge -x xsa_finish 2>/dev/null || true)
  pids="${pids//$'\n'/ }"
  for p in $pids; do
    if tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -qF "$ROOT"; then
      echo "SURVIVOR pid=$p $(tr '\0' ' ' < /proc/$p/cmdline)"
      exit 1
    fi
  done
  echo "NO_SURVIVORS checked=scatter_sort,cross_lcp_merge,xsa_finish root=$ROOT"
}
trap assert_no_survivors EXIT

run_triad() { # $1=case dir, $2=finish log name, $3=kill_after_fraction (0 = no kill), $4=final bytes
  local d=$1 log=$2 frac=$3 bytes=$4
  mkdir -p "$d/merged" "$d/bands"
  "$STAGES/xsa_finish" --corpus "$CORPUS" --sentinel-remap "$d/remap.bin" >"$d/remap.log" 2>&1
  : >"$d/checkpoint.journal"
  export XSA_CHECKPOINT_JOURNAL="$d/checkpoint.journal"
  export XSA_FINISH_SPOOL=1
  export CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=1100 XSA_TAPE_WAIT_S=inf
  unset XSA_KILL_AT XSA_WORK_ROOT || true
  "$STAGES/scatter_sort" --corpus "$CORPUS" --remap "$d/remap.bin" --scratch "$d/waves" \
      --tape-bands "$d/bands" --pending-bands 8 --budget-bytes 12884901888 \
      --memory-gb 24 --threads "$THREADS" >"$d/order.log" 2>&1 &
  local order=$!
  "$STAGES/cross_lcp_merge" --binbwt "$CORPUS" --remap "$d/remap.bin" \
      --out-prefix "$d/merged/frag" --threads "$THREADS" --tape-bands "$d/bands" \
      >"$d/emit.log" 2>&1 &
  local emit=$!
  local finish_cmd=("$STAGES/xsa_finish" --prefix "$d/merged/frag" --corpus "$CORPUS"
      --remap "$d/remap.bin" --output "$d/out.sxi2" --threads "$THREADS"
      --poll-timeout 0 --with-chi --lcp-bands "$d/bands")
  "${finish_cmd[@]}" >"$d/$log" 2>&1 &
  local fin=$!
  if [ "$frac" != "0" ]; then
    # Kill AFTER the run's first durable finish checkpoint (that is the resume
    # point; earlier there is nothing to resume from) plus a few seconds of
    # post-checkpoint work, so the retained prefix is ahead of the checkpoint and
    # the truncation path is exercised.
    local waited=0
    while kill -0 $fin 2>/dev/null; do
      if grep -q "^FINISH_CHECKPOINT " "$d/$log" 2>/dev/null; then break; fi
      sleep 0.2; waited=$((waited+1)); [ $waited -gt 15000 ] && { echo "KILL_TIMEOUT no checkpoint after $((waited/5))s"; exit 1; }
    done
    sleep 5
    echo "G4_KILL after_checkpoint partial_bytes=$(stat -c %s "$d/out.sxi2.partial")"
    kill -9 $fin; wait $fin 2>/dev/null || true
    echo "G4_KILLED finish_pid=$fin rc=-9"
    # RESTART the same finish command against the retained scratch.
    "${finish_cmd[@]}" >"$d/finish-resume.log" 2>&1
    echo "G4_RESTART rc=0 log=$d/finish-resume.log"
  fi
  local finrc=0
  wait $fin 2>/dev/null || finrc=$?
  wait $emit; wait $order
  return 0
}

echo "G4 corpus=$CORPUS stages=$STAGES root=$ROOT threads=$THREADS"
run_triad "$ROOT/clean" finish-clean.log 0 0
CLEAN_SHA=$(sha256sum "$ROOT/clean/out.sxi2" | cut -d' ' -f1)
CLEAN_BYTES=$(stat -c %s "$ROOT/clean/out.sxi2")
echo "G4_CLEAN sha256=$CLEAN_SHA bytes=$CLEAN_BYTES"
run_triad "$ROOT/killed" finish-killed.log 80 "$CLEAN_BYTES" 
RES_SHA=$(sha256sum "$ROOT/killed/out.sxi2" | cut -d' ' -f1)
RES_BYTES=$(stat -c %s "$ROOT/killed/out.sxi2")
echo "G4_RESUMED sha256=$RES_SHA bytes=$RES_BYTES"
[ "$CLEAN_SHA" = "$RES_SHA" ] && echo "G4_BYTE_IDENTICAL=1" || { echo "G4_BYTE_IDENTICAL=0"; exit 1; }
grep -E "^RESUME_MODE " "$ROOT/killed/finish-resume.log" || { echo "NO_RESUME_MODE"; exit 1; }
grep -E "^WORK_AVOIDED_SUMMARY " "$ROOT/killed/finish-resume.log" || true
grep -E "^(CONTAINER_FINAL|CONTAINER_STREAM mode=declare)" "$ROOT/killed/finish-resume.log" | head -3
echo G4_PASS
