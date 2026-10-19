#!/usr/bin/env bash
# prefetch2 harness, part 1: PRODUCE. One producer run per (corpus, producer
# env) whose output a set of finish arms then share - the arms differ only in the
# finish's read discipline, so the producer must be held fixed byte for byte.
#
#   bash tools/binbwt/pf2_produce.sh CORPUS TAG [live_cap_slots] [threads]
#
# FILE MODE (no CROSS_V2_PIECES): the producer publishes the complete piece set,
# the finish reads it in band order, and the same pristine output can be copied
# for as many finish arms as we want. The streaming shape is measured separately
# (pf2_stream.sh) because it cannot be re-run without re-running the producer.
set -uo pipefail
CORPUS="${1:?corpus}"; TAG="${2:?tag}"; CAP="${3:-250000000}"; TH="${4:-96}"
BIN=/mnt/nvme2n1/erikg/pf2-build/binbwt
REMAP=/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin
ROOT=/mnt/nvme2n1/erikg/pf2-$TAG
mkdir -p "$ROOT"; rm -rf "$ROOT"/frag* "$ROOT"/binbwt-windows "$ROOT"/product.sxi2
export CROSS_POOL_SLOTS="${CROSS_POOL_SLOTS:-33554432}"
PHASE_CAP_KB="${PHASE_CAP_KB:-8388608}"
unset CROSS_V2_PIECES
# PIECE WINDOW: the streaming wire's flow-control window. The producer blocks in
# v2_flow_wait until the consumer declares band k consumed, so a PRODUCER-ONLY
# run (the arms share one pristine output) must be allowed to run past the whole
# band count: 5616 bands at 1 GB, 590 at 100 MB. The wire bytes are identical
# either way - only the credit changes. (Learned the hard way: with the default
# window 1100 the producer sat in the credit wait forever and produced nothing.)
export CROSS_V2_PIECE_WINDOW="${CROSS_V2_PIECE_WINDOW:-1000000}"
echo "=== PRODUCE tag=$TAG corpus=$CORPUS cap=$CAP threads=$TH pool=${CROSS_POOL_SLOTS} $(date +%T) $(uptime|sed 's/.*load average/load/')"
( ulimit -v "$PHASE_CAP_KB"; /usr/bin/time -v -o "$ROOT/bin.time" "$BIN" --binbwt "$CORPUS" --remap "$REMAP" \
    --out-prefix "$ROOT/frag" --threads "$TH" --bin-live-cap "$CAP" 2> "$ROOT/bin.log" )
echo "BIN_RC=$?"
grep -E "CORPUS_RAM_CACHE|BIN_WORK" "$ROOT/bin.log" | tail -2
grep -E "Elapsed" "$ROOT/bin.time"
du -sh "$ROOT"/frag.pieces "$ROOT"/frag.rlebwt 2>/dev/null
echo "=== PRODUCE_DONE tag=$TAG $(date +%T)"
