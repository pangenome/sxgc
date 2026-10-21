#!/usr/bin/env bash
# kasai_finish_arm.sh - ONE finish arm on a pristine producer output + the
# prebuilt LCP column (the A/B driver for the kasai-finish charter).
#
#   bash tools/binbwt/kasai_finish_arm.sh ROOT TAG ARM COLUMN|none [THREADS] [CHI]
#
#   ARM = kasai   -> --lcp-column COLUMN                 (the new path)
#         old     -> PILE_FINISH_KASAI=0 --lcp-column C  (the SAME inputs, old walk)
#         audit   -> PILE_FINISH_KASAI_AUDIT=1           (both, asserted equal)
#   PRISTINE (env) = the producer output directory holding frag.pieces and
#   frag.rlebwt; every arm gets HARDLINK copies (the finish unlinks the piece
#   files as it consumes them, so an arm may never share them with another).
#   EXTRA (env) = additional env assignments for the finish, e.g.
#   "PILE_FINISH_POOL=96".
#
# A mismatch against $BANK is a hard failure of the arm, never a footnote.
set -uo pipefail
ROOT="${1:?root}"; TAG="${2:?tag}"; ARM="${3:?arm}"; COL="${4:-none}"
TH="${5:-96}"; CHI="${6:-29716349}"
PRISTINE="${PRISTINE:?set PRISTINE to the producer output dir}"
CORPUS="${CORPUS:?set CORPUS}"
REMAP="${REMAP:?set REMAP}"
BANK="${BANK:-}"; PF="${PF:?set PF to the pile_finish binary}"
D="$ROOT/$ARM"
rm -rf "$D"; mkdir -p "$D"
cp -al "$PRISTINE/frag.pieces" "$D/frag.pieces"
cp -al "$PRISTINE/frag.rlebwt" "$D/frag.rlebwt"
cp -al "$PRISTINE/frag.rlebwt.meta" "$D/frag.rlebwt.meta" 2>/dev/null
ENVARM=()
case "$ARM" in
  kasai) ENVARM=(PILE_FINISH_KASAI=1) ;;
  old)   ENVARM=(PILE_FINISH_KASAI=0) ;;
  audit) ENVARM=(PILE_FINISH_KASAI_AUDIT=1) ;;
  *) echo "unknown arm $ARM" >&2; exit 2 ;;
esac
COLARG=(); [ "$COL" != none ] && COLARG=(--lcp-column "$COL")
LOAD=$(cut -d' ' -f1 /proc/loadavg)
echo "=== ARM tag=$TAG arm=$ARM col=$COL threads=$TH loadavg=$LOAD $(date +%T)"
env "${ENVARM[@]}" ${EXTRA:-} \
  /usr/bin/time -v -o "$D/finish.time" "$PF" --prefix "$D/frag" \
    --corpus "$CORPUS" --remap "$REMAP" --output "$D/product.sxi2" \
    --threads "$TH" --poll-timeout 7200 --wit-ram-budget 4294967296 \
    --chi-expect "$CHI" --keep-rlebwt "${COLARG[@]}" > /dev/null 2> "$D/finish.log"
RC=$?
ELAPSED=$(grep -oE "Elapsed \(wall clock\).*" "$D/finish.time" | head -1 | sed 's/.*: //')
MAXRSS=$(grep -oE "Maximum resident set size.*" "$D/finish.time" | grep -oE "[0-9]+")
CHI_OUT=$(grep -oE "PILE_FINISH_DONE chi=[0-9]+" "$D/finish.log" | head -1 | cut -d= -f2)
SWEEP=$(grep -oE "sweep wall=[0-9.]+" "$D/finish.log" | head -1 | cut -d= -f2)
LCE=$(grep -oE "SWEEP_PHASE_WALLS repair=[0-9.]+ lce=[0-9.]+" "$D/finish.log" | grep -oE "lce=[0-9.]+" | cut -d= -f2)
VS=$(grep -oE "PILE_FINISH_DONE.*verified_symbols=[0-9]+" "$D/finish.log" | grep -oE "verified_symbols=[0-9]+" | cut -d= -f2)
FP=$(grep -oE "SLIM_FP pile-finish-corpus .*" "$D/finish.log" | tail -1)
HPV=$(echo "$FP" | grep -oE "hash_probes=[0-9]+" | cut -d= -f2)
VSW=$(echo "$FP" | grep -oE "verified_symbols=[0-9]+" | cut -d= -f2)
CQ=$(echo "$FP" | grep -oE "queries=[0-9]+" | cut -d= -f2)
COLREAD=$(grep -oE "PILE_FINISH_LCP_COLUMN_READS .*" "$D/finish.log" | tail -1)
SHA=$(sha256sum "$D/product.sxi2" 2>/dev/null | cut -d' ' -f1)
CMP=na; [ -n "$BANK" ] && { cmp -s "$D/product.sxi2" "$BANK" && CMP=0 || CMP=1; }
LINE="TAG=$TAG ARM=$ARM RC=$RC CMP=$CMP CHI=$CHI_OUT SHA=$SHA WALL=$ELAPSED MAXRSS_KB=$MAXRSS \
SWEEP_WALL=$SWEEP LCE_WALL=$LCE SLIM_QUERIES=$CQ SLIM_VERIFIED=$VSW HASH_PROBES=$HPV LOADAVG=$LOAD"
printf '%s\n' "$LINE" | tee -a "$ROOT/results.tsv"
{
  echo "  $LINE"
  echo "  ${COLREAD:-column: not read}"
  echo "  $FP"
  grep -E "PILE_FINISH_LCP_COLUMN |PILE_FINISH_KASAI |SWEEP_PHASE_WALLS|PILE_FINISH_REGION lce-span" "$D/finish.log" | sed 's/^/  /'
} >> "$ROOT/results-detail.log"
echo "=== ARM_DONE tag=$TAG arm=$ARM rc=$RC cmp=$CMP"
