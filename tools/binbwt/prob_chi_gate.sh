#!/usr/bin/env bash
# PROB-CHI 1 GB gate: TIER T0 (full) vs T3 (hash-trust + claims checker), on ONE
# pristine producer output, against the banked container.
#
#   bash tools/binbwt/prob_chi_gate.sh [ROOT] [THREADS]
#
# Produces, per arm, the finish journal + the container, and cmp/sha256 against
# the bank. A mismatch is a hard failure of the arm, never a footnote. The
# checker arms also run the standalone claims pass (`--check-claims`).
#
# Every wall line is prefixed with `uptime`, because this box carries the
# operator's live 100 GB rung: walls here are CEILINGS, not quiet-box numbers.
set -uo pipefail
ROOT="${1:-/mnt/nvme2n1/erikg/pc-1b}"
TH="${2:-96}"
BIN=/mnt/nvme2n1/erikg/pc-build/pile_finish
CORPUS=/mnt/nvme2n1/erikg/extcols-scratch/mergperf-1b-snap.txt
REMAP=/mnt/nvme2n1/erikg/sol7e-input-1b/remap-256.bin
CHUNKS=/mnt/nvme2n1/erikg/sol7e-input-1b/chunks
TOTAL=1000000665
BANK=/mnt/nvme2n1/erikg/binbwt-gate-1b-t96/product.sxi2   # 467,814,822 B, chi=283296933
PROD="$ROOT/prod"
mkdir -p "$ROOT/logs"

say() { printf '%s %s\n' "$(date +%T)" "$*" | tee -a "$ROOT/logs/gate.log"; }

# PRODUCE: the same binned producer the banked fixture came from
# (tools/binbwt/pf2_produce.sh, FILE MODE: CROSS_V2_PIECES unset, so the producer
# publishes the COMPLETE piece set and the three finish arms can share it).
# The chunk k-way merge is NOT this producer: it emits the dense ssa/ssa_t
# columns (D2 violation) instead of v2 band pieces. Learned by doing.
if [ ! -f "$PROD/frag.rlebwt.meta" ]; then
  say "PRODUCE_START $(uptime | sed 's/.*load average/load/')"
  mkdir -p "$PROD"
  ( ulimit -v "${PHASE_CAP_KB:-16777216}"; CROSS_POOL_SLOTS=33554432 CROSS_V2_PIECE_WINDOW=1000000 \
    /usr/bin/time -v -o "$ROOT/logs/produce.time" \
    /mnt/nvme2n1/erikg/pc-build/binbwt --binbwt "$CORPUS" --remap "$REMAP" \
      --out-prefix "$PROD/frag" --threads "$TH" --bin-live-cap 250000000 \
      > "$ROOT/logs/produce.log" 2>&1 )
  say "PRODUCE_RC=$? $(grep -oE 'Elapsed .*' "$ROOT/logs/produce.time" | tail -1)"
  grep -E "BIN_WORK" "$ROOT/logs/produce.log" | tail -1 | sed 's/^/  [produce] /' | tee -a "$ROOT/logs/gate.log"
else
  say "PRODUCE_SKIP pristine exists"
fi

arm() {   # arm TAG [env...]
  local tag="$1"; shift
  local dir="$ROOT/arm-$tag"
  rm -rf "$dir"; mkdir -p "$dir"
  # HARDLINKS: the finish only READS the pristine and unlinks its own links.
  cp -al "$PROD"/frag.rlebwt "$PROD"/frag.rlebwt.meta "$PROD"/frag.pieces "$dir"/
  say "ARM_START tag=$tag $* $(uptime | sed 's/.*load average/load/')"
  ( cd "$dir" && env "$@" /usr/bin/time -v -o finish.time \
      "$BIN" --prefix frag --corpus "$CORPUS" --remap "$REMAP" --output product.sxi2 \
      --threads "$TH" --poll-timeout 3600 --wit-ram-budget 4294967296 \
      --chi-expect 283296933 > /dev/null 2> finish.log )
  local rc=$?
  local chi; chi=$(grep -oE "PILE_FINISH_DONE chi=[0-9]+" "$dir/finish.log" | head -1)
  local cmpv=NA sha=-
  if [ -f "$dir/product.sxi2" ]; then
    sha=$(sha256sum "$dir/product.sxi2" | cut -d' ' -f1)
    cmp -s "$dir/product.sxi2" "$BANK" && cmpv=0 || cmpv=1
  fi
  say "ARM_DONE tag=$tag rc=$rc $chi cmp_bank=$cmpv sha=$sha $(grep -oE 'Elapsed .*' "$dir/finish.time" | tail -1)"
  grep -E "PILE_FINISH_TIER|SWEEP_PHASE_WALLS|SLIM_FP pile-finish-corpus|PILE_FINISH_CLAIMS|PILE_FINISH_DONE" \
    "$dir/finish.log" | sed "s/^/  [$tag] /" | tee -a "$ROOT/logs/gate.log"
}

# ---- T0: today's FULL tier (the constructor byte-verifies every answer)
arm t0 PILE_FINISH_PROBE_DEGREES=1
# ---- T3a: hash-trust + position-bucketed claims spool, then the standalone checker
arm t3spool PILE_FINISH_LCE_TRUST=1 PILE_FINISH_CLAIMS_DIR="$ROOT/claims" PILE_FINISH_PROBE_DEGREES=1
say "CHECKER_START $(uptime | sed 's/.*load average/load/')"
/usr/bin/time -v -o "$ROOT/logs/checker.time" \
  "$BIN" --check-claims "$ROOT/claims" --corpus "$CORPUS" --remap "$REMAP" --threads "$TH" \
  > /dev/null 2> "$ROOT/logs/checker.log"
say "CHECKER_RC=$? $(grep -oE 'Elapsed .*' "$ROOT/logs/checker.time" | tail -1)"
grep -E "PILE_FINISH_CHECK_CLAIMS|FATAL" "$ROOT/logs/checker.log" | sed 's/^/  [checker] /' | tee -a "$ROOT/logs/gate.log"
# ---- T3b: hash-trust + FUSED in-worker checking (the pile-scale shape)
arm t3fused PILE_FINISH_LCE_TRUST=2
# ---- FLOOR: hash-trust with NO check at all. A MEASUREMENT ARM, not a tier:
# it prices the constructor's floor so the checker's own price is separable.
arm floor PILE_FINISH_LCE_TRUST=3
say "GATE_DONE $(uptime | sed 's/.*load average/load/')"
