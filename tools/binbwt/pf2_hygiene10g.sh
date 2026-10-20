#!/usr/bin/env bash
# THE DECISIVE PART-C MEASUREMENT: the 10 GB pile-regime HYGIENE A/B.
#
#   arm hyg0 : prefetch sweep ON, hygiene OFF  - the window files, the witness
#              spool and the rlebwt stay in the page cache after they are dead,
#              i.e. the pile's own scratch churns the corpus out.
#   arm hyg1 : prefetch sweep ON, hygiene ON   - every dead scratch page is told
#              so (DONTNEED) the moment it stops being needed, so the corpus is
#              the thing that keeps the cache.
#
# Both arms: the live 10 GB shape (producer and finish CONCURRENT, streaming
# wire, 96 threads, the default derived bin live cap under the phase ulimit), the
# corpus dropped from the page cache immediately before the band sweep
# (PILE_FINISH_COLD_CORPUS=1), the residency journal on, and NOTHING else
# different. The readout is the finish wall + the corpus's residency per phase.
#
#   bash tools/binbwt/pf2_hygiene10g.sh hyg0|hyg1 [threads]
#
# DETACHED by design (the caller nohups this): the producer alone writes ~80 GB of
# window files at 10 GB, and the finish reads a 10 GB corpus ~28 times over.
set -uo pipefail
ARM="${1:?hyg0|hyg1}"; TH="${2:-96}"
BB=/mnt/nvme2n1/erikg/pf2-build/binbwt
PFB=/mnt/nvme2n1/erikg/pf2-build/pile_finish
CORPUS=/mnt/nvme2n1/erikg/extcols-scratch/real10b-ref/pile-10b-snap.txt
REMAP=/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin
ROOT=/mnt/nvme2n1/erikg/pf2-10g-$ARM
mkdir -p "$ROOT"; rm -rf "$ROOT"/frag* "$ROOT"/binbwt-windows "$ROOT"/product.sxi2
case "$ARM" in
  hyg0) HYG=0 ;;
  hyg1) HYG=1 ;;
  *) echo "unknown arm $ARM (hyg0|hyg1)" >&2; exit 2 ;;
esac
# The live 10 GB shape: bounded corpus cache, streaming piece wire, phase caps.
# PHASE CAP: 60 GiB, not the live run.s 100 GiB, because the box carries two live
# 10 GB arms of the operator.s A/B and 108 GiB was available when this launched.
# BOTH arms run under the identical cap, so the A/B is internally comparable; the
# reconciliation against the live arms carries the cap difference in its label.
PHASE_CAP_KB="${PHASE_CAP_KB:-62914560}"
export CROSS_POOL_SLOTS=33554432 CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=1100
export PILE_FINISH_PREFETCH=1 PILE_FINISH_PREFETCH_ASSIST=populate
export PILE_FINISH_PREFETCH_JOBS=2 PILE_FINISH_PREFETCH_BUDGET=33554432
export PILE_FINISH_COLD_CORPUS=1 PILE_FINISH_CACHE_JOURNAL=1 PILE_FINISH_HYGIENE="$HYG"
export BIN_WINDOW_DONTNEED="$HYG"
START=$(date +%s)
echo "=== HYGIENE_AB arm=$ARM hygiene=$HYG threads=$TH cap_kb=$PHASE_CAP_KB start=$(date +%T) $(uptime|sed 's/.*load average/load/')"
(
  ulimit -v "$PHASE_CAP_KB"
  /usr/bin/time -v -o "$ROOT/bin.time" "$BB" --binbwt "$CORPUS" --remap "$REMAP" \
    --out-prefix "$ROOT/frag" --threads "$TH" 2> "$ROOT/bin.log"
  echo "BIN_RC=$?" >> "$ROOT/status"
) &
BP=$!
(
  ulimit -v "$PHASE_CAP_KB"
  /usr/bin/time -v -o "$ROOT/finish.time" "$PFB" --prefix "$ROOT/frag" --corpus "$CORPUS" \
    --remap "$REMAP" --output "$ROOT/product.sxi2" --threads "$TH" \
    --poll-timeout 2147483647 --wit-ram-budget 8589934592 2> "$ROOT/finish.log"
  echo "FINISH_RC=$?" >> "$ROOT/status"
) &
FP=$!
wait $BP; wait $FP
END=$(date +%s)
echo "=== HYGIENE_AB_DONE arm=$ARM total_wall_s=$((END-START)) end=$(date +%T)"
cat "$ROOT/status" 2>/dev/null
grep -E "Elapsed|Maximum resident" "$ROOT/bin.time" "$ROOT/finish.time" 2>/dev/null
grep -oE "PILE_FINISH_CACHE_RESIDENCY label=[a-z0-9-]+ .*resident_pct=[0-9.]+" "$ROOT/finish.log" | head -20
grep -oE "PILE_FINISH_PREFETCH jobs=.*" "$ROOT/finish.log" | tail -1
grep -oE "PILE_FINISH_DONE.*" "$ROOT/finish.log"
grep -oE "SLIM_FP pile-finish-corpus.*verified_symbols=[0-9]+" "$ROOT/finish.log"
echo "=== HYGIENE_AB_REPORTED arm=$ARM"
