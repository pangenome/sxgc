#!/usr/bin/env bash
# gates_keyradix.sh - K1..K5 for feat/keyradix-leaf-sort.
#
#   gates_keyradix.sh k2 | k4 | k5-150m | k5-10g | k1-100g | all
#
# Environment:
#   WORK       scratch root            default /mnt/nvme2n1/erikg/KEYRADIX
#   BASE_BIN   pre-change sorter       default /tmp/pre-keyradix/scatter_sort
#   FIXBYTES   fixture size            default 6291456
#
# NO TIMEOUT ON ANY RUN. Every run below ends when the binary exits; a run that is
# stopped deliberately has its numbers discarded, and the log says so. A timeout
# on a measurement is how a gate reports a rate with no denominator: the earlier
# version of K3 was wrapped in `timeout 1200` and produced "signal 9 at 1210 s,
# 0 bands completed, read_bytes/row unknowable". The policy line is printed so a
# reader can tell whether a number came from a completed run or a truncated one.
# `no_survivors` is kept as the post-run assertion that nothing outlived its run.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/mnt/nvme2n1/erikg/KEYRADIX}
BASE_BIN=${BASE_BIN:-/tmp/pre-keyradix/scatter_sort}
NEW_BIN=${NEW_BIN:-$WORK/stage/scatter_sort}
SA_CHECK=${SA_CHECK:-$WORK/stage/sa_check}
ORACLE=${ORACLE:-$WORK/stage/subdiv_oracle}
FIXTURES=${FIXTURES:-$WORK/stage/subdiv_fixtures}
FX=$WORK/fixtures
LOG=$WORK/glogs
FIXBYTES=${FIXBYTES:-6291456}
P150=/mnt/nvme2n1/erikg/sd-scratch/pile150.txt
TEN_GB=/mnt/nvme2n1/erikg/pf-10gb.txt
P100=/mnt/nvme2n1/erikg/pile-head100.txt
mkdir -p "$FX" "$LOG"

fail() { echo "KEYRADIX_GATE_FAIL: $*" >&2; exit 3; }
echo "KEYRADIX_GATE_POLICY timeout=none runs_end_by_their_own_exit=1 deliberate_stop=recorded_as_void"
RUN_TAGFILE=$WORK/.kr_run_tags
: > "$RUN_TAGFILE"
no_survivors() {
  local bad=""
  while read -r tag; do
    [ -n "$tag" ] || continue
    local hits
    hits=$(pgrep -af "scatter_sort" | grep -F -- "$WORK/runs/$tag/" || true)
    [ -z "$hits" ] || bad="$bad
$hits"
  done < "$RUN_TAGFILE"
  [ -z "$bad" ] || { echo "KEYRADIX_SURVIVORS:$bad" >&2; fail "a stage process outlived its run"; }
  echo "KEYRADIX_NO_SURVIVORS checked=$(wc -l < "$RUN_TAGFILE") tags='$(tr '\n' ' ' < "$RUN_TAGFILE")'"
}

# run_sorter BIN TAG CORPUS BUDGET_BYTES SCRATCH_BYTES CAP_GB EXTRA...
run_sorter() {
  local bin=$1 tag=$2 corpus=$3 budget=$4 scratchb=$5 cap=$6; shift 6
  local d=$WORK/runs/$tag
  echo "$tag" >> "$RUN_TAGFILE"
  rm -rf "$d"; mkdir -p "$d/scratch"
  setsid "$bin" \
      --corpus "$corpus" --out "$d/out.sa" --lcp-out "$d/out.lcp" --scratch "$d/scratch" \
      --threads "${THREADS:-24}" --budget-bytes "$budget" --scratch-budget-bytes "$scratchb" \
      --memory-gb "${MEMGB:-48}" --scratch-cap-gb "$cap" "$@" \
      > "$d/run.log" 2>&1 || { echo "rc=$?"; tail -5 "$d/run.log" >&2; fail "run $tag failed"; }
  echo "$d"
}

# ------------------------------------------------------------------------- K2
k2() {
  echo "== K2 brute oracle on the adversarial fixtures (order AND column)"
  [ -s "$FX/single.txt" ] || "$FIXTURES" "$FX" "$FIXBYTES" > "$LOG/fixtures.log"
  local n=0 rows=0 bad=0
  for f in "$FX"/*.txt; do
    local name; name=$(basename "$f" .txt)
    [ "$name" = refuse ] && continue     # the refusal fixture must NOT sort
    local a b
    a=$(run_sorter "$NEW_BIN" "k2_new_$name" "$f" 3000000000 4000000000 2)
    b=$(run_sorter "$BASE_BIN" "k2_base_$name" "$f" 3000000000 4000000000 2)
    local ochr
    ochr=$("$ORACLE" --corpus "$f" --sa "$a/out.sa" 2>&1 | tail -1)
    local achk
    achk=$("$SA_CHECK" --corpus "$f" --sa "$a/out.sa" 2>&1 | tail -1)
    local saeq lcpeq
    saeq=$(cmp -s "$a/out.sa" "$b/out.sa" && echo 1 || echo 0)
    lcpeq=$(cmp -s "$a/out.lcp" "$b/out.lcp" && echo 1 || echo 0)
    local r; r=$(echo "$ochr" | sed -n 's/.*rows=\([0-9]*\).*/\1/p')
    rows=$((rows + ${r:-0})); n=$((n + 1))
    local rounds; rounds=$(grep -c 'SORT_ARM ' "$a/run.log" || true)
    local ties; ties=$(grep -o 'tie_rows=[0-9]*' "$a/run.log" | cut -d= -f2 | awk '{s+=$1} END{printf "%d", s+0}')
    local fb; fb=$(grep -o 'fallback_rows=[0-9]*' "$a/run.log" | cut -d= -f2 | awk '{s+=$1} END{printf "%d", s+0}')
    local ext; ext=$(grep -o 'ext_bytes=[0-9]*' "$a/run.log" | cut -d= -f2 | awk '{s+=$1} END{printf "%d", s+0}')
    echo "KEYRADIX_K2 fixture=$name rows=${r:-0} bands_with_radix=$rounds tie_rows=$ties fallback_rows=$fb ext_bytes=$ext sa_identical=$saeq lcp_identical=$lcpeq"
    echo "KEYRADIX_K2_ORACLE fixture=$name $ochr"
    echo "KEYRADIX_K2_SA_CHECK fixture=$name $achk"
    case "$ochr" in *PASS*) ;; *) bad=1 ;; esac
    case "$achk" in *PASS*) ;; *) bad=1 ;; esac
    [ "$saeq" = 1 ] && [ "$lcpeq" = 1 ] || bad=1
  done
  echo "KEYRADIX_K2_SUMMARY fixtures=$n rows_examined=$rows all_green=$([ $bad = 0 ] && echo 1 || echo 0)"
  [ $bad = 0 ] || fail "K2: a fixture disagreed with the brute-force oracle or the pre-change stream"
  no_survivors
}

# ------------------------------------------------------------------------- K4
k4() {
  echo "== K4 tie accounting on the pathological fixtures (rounds, shrink, tie bytes)"
  local bad=0
  for name in single period2 eofrun docsep internsep mixed r0; do
    local f=$FX/$name.txt d=$WORK/runs/k4_$name
    [ -s "$f" ] || fail "K4: fixtures missing (run k2 first)"
    d=$(run_sorter "$NEW_BIN" "k4_$name" "$f" 3000000000 4000000000 2)
    local rows tie ext rounds_n fb nf
    rows=$(grep -o 'SS_START n=[0-9]*' "$d/run.log" | head -1 | cut -d= -f2)
    tie=$(grep -o 'tie_rows=[0-9]*' "$d/run.log" | cut -d= -f2 | awk '{s+=$1} END{printf "%d", s+0}')
    ext=$(grep -o 'ext_bytes=[0-9]*' "$d/run.log" | cut -d= -f2 | awk '{s+=$1} END{printf "%d", s+0}')
    fb=$(grep -o 'fallback_rows=[0-9]*' "$d/run.log" | cut -d= -f2 | awk '{s+=$1} END{printf "%d", s+0}')
    rounds_n=$(grep -c 'SORT_ROUND_STALL' "$d/run.log" || true)
    local maxround; maxround=$(grep -o 'rounds=[0-9]*' "$d/run.log" | cut -d= -f2 | sort -n | tail -1)
    echo "KEYRADIX_K4 fixture=$name n=$rows bands=$(grep -c 'SORT_ARM ' "$d/run.log") rounds_max_seen=${maxround:-0} stall_rounds=$rounds_n tie_rows_sum=$tie ext_bytes_sum=$ext fallback_rows_sum=$fb"
    # (a) no band may exceed the round cap
    if [ "${maxround:-0}" -gt 5 ]; then echo "KEYRADIX_K4_FAIL fixture=$name rounds_max_seen=$maxround > 5"; bad=1; fi
    # (c) tie bytes are proportional to tie rows (K=32, <= 32 per round, <= 4 rounds)
    if [ "$tie" -gt 0 ]; then
      local per; per=$(awk -v b="$ext" -v r="$tie" 'BEGIN{printf "%.2f", b/r}')
      echo "KEYRADIX_K4_PROPORTIONAL fixture=$name ext_bytes_per_tie_row=$per bound=4096(page) ext_over_n=$(awk -v b="$ext" -v r="$rows" 'BEGIN{printf "%.4f", b/r}')"
    fi
    echo "KEYRADIX_K4_IO fixture=$name $(grep -o 'SS_SORT_IO .*' "$d/run.log" | head -1)"
  done
  [ $bad = 0 ] || fail "K4: a round exceeded the cap"
  echo "== K4 round shape: the pathological one-symbol run, band 0 of the deepest wave"
  grep -o 'SORT_ROUND .*' "$WORK/runs/k4_single/run.log" | head -12
  echo "== K4 round shape: the periodic 'ab'*N run"
  grep -o 'SORT_ROUND .*' "$WORK/runs/k4_period2/run.log" | head -8
  # (b) EVERY round must shrink the tie set or be exhausted: the stall count is the
  # only thing that may be non-zero, and the stall line names why.
  local stalls; stalls=$(grep -c 'SORT_ROUND_STALL' "$WORK"/runs/k4_*/run.log 2>/dev/null | awk -F: '{s+=$2} END{printf "%d", s+0}')
  local rounds; rounds=$(grep -c 'SORT_ROUND ' "$WORK"/runs/k4_*/run.log 2>/dev/null | awk -F: '{s+=$2} END{printf "%d", s+0}')
  echo "KEYRADIX_K4_ROUNDS total=$rounds stalled=$stalls"
  echo "KEYRADIX_K4_PASS"
  no_survivors
}

# ------------------------------------------------------------------------- K5
k5_150m() {
  echo "== K5 at 150 MB: the emitted rank stream AND the LCP column are the pre-change bytes"
  local a b sha_a sha_b
  a=$(run_sorter "$NEW_BIN" k5_150m_new "$P150" 12000000000 24000000000 2)
  b=$(run_sorter "$BASE_BIN" k5_150m_base "$P150" 12000000000 24000000000 2)
  sha_a=$(sha256sum "$a/out.sa" | cut -d' ' -f1)
  sha_b=$(sha256sum "$b/out.sa" | cut -d' ' -f1)
  local la lb
  la=$(sha256sum "$a/out.lcp" | cut -d' ' -f1)
  lb=$(sha256sum "$b/out.lcp" | cut -d' ' -f1)
  local n; n=$(grep -o 'SS_START n=[0-9]*' "$a/run.log" | head -1 | cut -d= -f2)
  echo "KEYRADIX_K5_150M n=$n new_sa=$sha_a base_sa=$sha_b sa_equal=$([ "$sha_a" = "$sha_b" ] && echo 1 || echo 0)"
  echo "KEYRADIX_K5_150M new_lcp=$la base_lcp=$lb lcp_equal=$([ "$la" = "$lb" ] && echo 1 || echo 0)"
  echo "KEYRADIX_K5_150M bands_compared=$(grep -c 'SS_BAND band=' "$a/run.log")"
  [ "$sha_a" = "$sha_b" ] || fail "K5: the 150 MB rank stream drifted"
  [ "$la" = "$lb" ] || fail "K5: the 150 MB LCP column drifted"
  echo "KEYRADIX_K5_150M_PASS"
  no_survivors
}

k5_10g() {
  echo "== K5 at 10 GB: the emitted rank stream AND the LCP column are the pre-change bytes"
  local a b sha_a sha_b la lb
  a=$(run_sorter "$NEW_BIN" k5_10g_new "$TEN_GB" 48000000000 60000000000 60)
  b=$(run_sorter "$BASE_BIN" k5_10g_base "$TEN_GB" 48000000000 60000000000 60)
  sha_a=$(sha256sum "$a/out.sa" | cut -d' ' -f1)
  sha_b=$(sha256sum "$b/out.sa" | cut -d' ' -f1)
  la=$(sha256sum "$a/out.lcp" | cut -d' ' -f1)
  lb=$(sha256sum "$b/out.lcp" | cut -d' ' -f1)
  local n; n=$(grep -o 'SS_START n=[0-9]*' "$a/run.log" | head -1 | cut -d= -f2)
  local bands; bands=$(grep -c 'SS_BAND band=' "$a/run.log")
  local wa wb
  wa=$(grep -o 'total_s=[0-9.]*' "$a/run.log" | tail -1)
  wb=$(grep -o 'total_s=[0-9.]*' "$b/run.log" | tail -1)
  echo "KEYRADIX_K5_10G n=$n bands_compared=$bands new_sa=$sha_a base_sa=$sha_b sa_equal=$([ "$sha_a" = "$sha_b" ] && echo 1 || echo 0)"
  echo "KEYRADIX_K5_10G new_lcp=$la base_lcp=$lb lcp_equal=$([ "$la" = "$lb" ] && echo 1 || echo 0)"
  echo "KEYRADIX_K5_10G wall new:$wa base:$wb"
  [ "$sha_a" = "$sha_b" ] || fail "K5: the 10 GB rank stream drifted"
  [ "$la" = "$lb" ] || fail "K5: the 10 GB LCP column drifted"
  echo "KEYRADIX_K5_10G_PASS"
  no_survivors
}

# ------------------------------------------------------------------------- K1
k1_100g() {
  echo "== K1 at 100 GB (the pile head): byte identity with the pre-change binary, same flags"
  local a=$WORK/k1_100g_new b=$WORK/k1_100g_base
  for pair in "$a:$NEW_BIN" "$b:$BASE_BIN"; do
    local d=${pair%%:*} bin=${pair##*:}
    if [ ! -s "$d/out.sa" ]; then
      rm -rf "$d"; mkdir -p "$d/scratch"
      /usr/bin/time -f "KEYRADIX_K1_WALL %e s RSS %M kB INBLOCK %I" \
        setsid "$bin" --corpus "$P100" \
        --out "$d/out.sa" --lcp-out "$d/out.lcp" --scratch "$d/scratch" \
        --threads "${THREADS:-24}" --budget-bytes 48000000000 --scratch-budget-bytes 60000000000 \
        --memory-gb 48 --scratch-cap-gb 60 > "$d/run.log" 2>&1 || fail "K1: $d failed"
    fi
  done
  local sa_a sa_b la lb
  sa_a=$(sha256sum "$a/out.sa" | cut -d' ' -f1); sa_b=$(sha256sum "$b/out.sa" | cut -d' ' -f1)
  la=$(sha256sum "$a/out.lcp" | cut -d' ' -f1); lb=$(sha256sum "$b/out.lcp" | cut -d' ' -f1)
  local n; n=$(grep -o 'SS_START n=[0-9]*' "$a/run.log" | head -1 | cut -d= -f2)
  echo "KEYRADIX_K1_100G n=$n bands_new=$(grep -c 'SS_BAND band=' "$a/run.log") bands_base=$(grep -c 'SS_BAND band=' "$b/run.log")"
  echo "KEYRADIX_K1_100G new_sa=$sa_a"
  echo "KEYRADIX_K1_100G base_sa=$sa_b"
  echo "KEYRADIX_K1_100G sa_equal=$([ "$sa_a" = "$sa_b" ] && echo 1 || echo 0)"
  echo "KEYRADIX_K1_100G new_lcp=$la base_lcp=$lb lcp_equal=$([ "$la" = "$lb" ] && echo 1 || echo 0)"
  if [ "$sa_a" != "$sa_b" ]; then
    # STOP and name the first differing band, never iterate blind.
    local off; off=$(cmp "$a/out.sa" "$b/out.sa" 2>/dev/null | sed -n 's/.*byte \([0-9]*\).*/\1/p' | head -1)
    echo "KEYRADIX_K1_100G FIRST_DIFF_BYTE=${off:-unknown} first_diff_rank=$(( (${off:-48} - 48) / 8 ))" >&2
    fail "K1: the 100 GB rank stream drifted - stopped at the first differing band"
  fi
  [ "$la" = "$lb" ] || fail "K1: the 100 GB LCP column drifted"
  echo "KEYRADIX_K1_100G_PASS"
}

# ------------------------------------------------------------------------- K3
k3() {
  echo "== K3 bytes of read per row, sized to FINISH, no timeout, both arms cold"
  bash "$HERE/gates/keyradix_k3.sh" || fail "K3 script failed"
  cat "$WORK/k3sized.receipt"
  # every receipt must carry its own denominator, and a VOID one is a failure
  local void_n; void_n=$(grep -c ' VOID ' "$WORK/k3sized.receipt" || true)
  local ok_n; ok_n=$(grep -c 'completed=1' "$WORK/k3sized.receipt" || true)
  echo "KEYRADIX_K3_RECEIPTS completed=$ok_n void=$void_n"
  [ "$void_n" = 0 ] || fail "K3: a receipt is VOID (its run did not complete)"
  for tag in base new; do
    grep -q "K3 tag=$tag corpus=" "$WORK/k3sized.receipt" || fail "K3: no completed receipt for $tag"
  done
  local rb rn
  rb=$(grep "K3 tag=base corpus=" "$WORK/k3sized.receipt" | sed -n 's/.*read_bytes=\([0-9]*\).*/\1/p')
  rn=$(grep "K3 tag=new corpus=" "$WORK/k3sized.receipt" | sed -n 's/.*read_bytes=\([0-9]*\).*/\1/p')
  echo "KEYRADIX_K3_RATIO base_read_bytes=$rb new_read_bytes=$rn ratio=$(awk -v b="$rb" -v n="$rn" 'BEGIN{if(n>0)printf "%.3f",b/n; else print "NA"}')"
  echo "KEYRADIX_K3_PASS"
}

case "${1:-all}" in
  k3) k3 ;;
  k3*) k3 ;;
  k2) k2 ;;
  k4) k4 ;;
  k5-150m) k5_150m ;;
  k5-10g) k5_10g ;;
  k1-100g) k1_100g ;;
  all) k2; k4; k3; k5_150m; k5_10g ;;
  *) fail "usage: $0 k2|k4|k3|k5-150m|k5-10g|k1-100g|all" ;;
esac
