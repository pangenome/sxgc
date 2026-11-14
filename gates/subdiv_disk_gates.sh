#!/usr/bin/env bash
# subdiv_disk_gates.sh - the position-window gates of RESULTS_SUBDIV_DISK.md.
#
#   gates/subdiv_disk_gates.sh d1|d2|d3|d4|d5|all
#
# Environment:
#   WORK=<scratch root>       default /mnt/nvme2n1/erikg/SUBDIV-DISK
#   BASE_REF=<pre-fix rev>    default origin/main (the binary that filled the pile)
#   CORPUS=<corpus for d2..d4>   default /mnt/nvme2n1/erikg/pf-10gb.txt
#   BUDGET=<the run's --budget-bytes>  default 96000000000
#   SCRATCH_GB / THREADS / MEM_GB as for the tape gates
#   PEAK_BUDGET=<pos_budget for d2>   default 20000000000 (20 GB)
#   TIGHT_BUDGET=<pos_budget for d4>  default 1000000000  (1 GB)
#   FIXBYTES=<fixture size>           default 6291456
#   RUN_TIMEOUT (s), default 21600
#
# Every product run is wrapped in `setsid timeout --kill-after=10` and the script
# asserts that no stage process survives it (scoped to this WORK's scratch dirs).
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/mnt/nvme2n1/erikg/SUBDIV-DISK}
BASE_REF=${BASE_REF:-origin/main}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pf-10gb.txt}
BUDGET=${BUDGET:-96000000000}
SCRATCH_GB=${SCRATCH_GB:-250}
THREADS=${THREADS:-96}
MEM_GB=${MEM_GB:-96}
PEAK_BUDGET=${PEAK_BUDGET:-20000000000}
TIGHT_BUDGET=${TIGHT_BUDGET:-1000000000}
FIXBYTES=${FIXBYTES:-6291456}
RUN_TIMEOUT=${RUN_TIMEOUT:-21600}
STAGE=$WORK/stage
FX=$WORK/fixtures
LOG=$WORK/logs
mkdir -p "$STAGE" "$FX" "$LOG"

CXX=${CXX:-g++}
CFLAGS="-O2 -std=c++17 -pthread"
fail() { echo "SUBDIV_DISK_GATE_FAIL: $*" >&2; exit 3; }

RUN_TAGFILE=$WORK/.run_tags
: > "$RUN_TAGFILE"
no_survivors() {
  local bad=""
  for tag in $(cat "$RUN_TAGFILE"); do
    local hits
    hits=$(pgrep -af "scatter_sort" | grep -F -- "$WORK/runs/$tag/scratch" || true)
    [ -z "$hits" ] || bad="$bad
$hits"
  done
  [ -z "$bad" ] || { echo "SUBDIV_DISK_SURVIVORS:$bad" >&2; fail "a stage process survived its timeout"; }
  echo "SUBDIV_DISK_NO_SURVIVORS checked=$(wc -w < "$RUN_TAGFILE") runs"
}

build() {
  echo "== BUILD stage=$STAGE"
  $CXX $CFLAGS "$HERE/xsa/runtime/tools/binbwt/scatter_sort.cpp" -o "$STAGE/scatter_sort" -latomic
  $CXX $CFLAGS "$HERE/xsa/runtime/tools/binbwt/sa_check.cpp"      -o "$STAGE/sa_check"
  $CXX $CFLAGS "$HERE/gates/subdiv_fixtures.c"                    -o "$STAGE/subdiv_fixtures"
  echo "SUBDIV_DISK_BUILD scatter_sort_sha=$(sha256sum "$STAGE/scatter_sort" | cut -d' ' -f1)"
  if [ ! -x "$STAGE/base_scatter_sort" ] || [ "${REBUILD_BASE:-0}" = 1 ]; then
    mkdir -p "$STAGE/base_src"
    git -C "$HERE" show "$BASE_REF:xsa/runtime/tools/binbwt/scatter_sort.cpp" > "$STAGE/base_src/scatter_sort.cpp"
    cp "$HERE"/xsa/runtime/tools/binbwt/subdiv_survey.h "$HERE"/xsa/runtime/tools/binbwt/subdiv_descent.h "$STAGE/base_src/"
    $CXX $CFLAGS "$STAGE/base_src/scatter_sort.cpp" -o "$STAGE/base_scatter_sort" -latomic
  fi
  echo "SUBDIV_DISK_BASE ref=$BASE_REF sha=$(sha256sum "$STAGE/base_scatter_sort" | cut -d' ' -f1)"
}

fixtures() {
  if [ ! -s "$FX/docsep.txt" ] || [ "$(stat -c %s "$FX/docsep.txt")" != "$FIXBYTES" ]; then
    rm -f "$FX"/*.txt
    "$STAGE/subdiv_fixtures" "$FX" "$FIXBYTES" > "$LOG/fixtures.log"
  fi
  echo "SUBDIV_DISK_FIXTURES dir=$FX bytes=$FIXBYTES n=$(ls "$FX"/*.txt | wc -l)"
}

# run_sorter BIN TAG CORPUS EXTRA... -> echoes the run dir; fails on a non-zero rc.
run_sorter() {
  local bin=$1 tag=$2 corpus=$3; shift 3
  local d=$WORK/runs/$tag
  echo "$tag" >> "$RUN_TAGFILE"
  rm -rf "$d"; mkdir -p "$d/scratch"
  local t0=$(date +%s)
  echo "SUBDIV_DISK_RUN tag=$tag corpus=$corpus bytes=$(stat -c %s "$corpus") extra='$*'" >&2
  setsid timeout --kill-after=10 "$RUN_TIMEOUT" "$bin" \
      --corpus "$corpus" --out "$d/out.sa" --scratch "$d/scratch" "$@" \
      > "$d/run.log" 2>&1 || { echo "rc=$?"; tail -5 "$d/run.log" >&2; fail "run $tag failed"; }
  local t1=$(date +%s)
  echo $((t1 - t0)) > "$d/wall_s"
  echo "$d"
}

sha_of() { sha256sum "$1/out.sa" | cut -d' ' -f1; }
peak_of() { grep -o 'SS_SUBDIV_POSPEAK .*' "$1/run.log" | grep -o 'peak_total=[0-9]*' | cut -d= -f2 | sort -n | tail -1; }
wins_of() { grep -c 'SS_SUBDIV_POSPEAK' "$1/run.log" || true; }
passes_of() { grep -o 'SS_SUBDIV_SCATTER passes=[0-9]*' "$1/run.log" | tail -1 | cut -d= -f2; }
posdir_of() { local n=0; [ -d "$1/scratch/subdiv_pos" ] && n=$(find "$1/scratch/subdiv_pos" -type f | wc -l); echo "$n"; }
posbytes_of() { local b=0; [ -d "$1/scratch/subdiv_pos" ] && b=$(find "$1/scratch/subdiv_pos" -type f -printf '%s\n' | awk '{s+=$1} END {print s+0}'); echo "$b"; }
peak_wins() { grep -o 'SS_SUBDIV_POSPEAK .*' "$1/run.log" | head -3 | sed 's/rule=.*//'; }

# --------------------------------------------------------------------------- D1
d1() {
  echo "== D1 fixture parity: pre-fix vs this build, auto AND forced-local arms, N fixtures"
  local n=0 rows=0
  for f in "$FX"/*.txt; do
    local name; name=$(basename "$f" .txt)
    [ "$name" = refuse ] && continue     # the refusal fixture must NOT sort
    for arm in auto local; do
      local x=(); [ "$arm" = local ] && x=(--subdiv-strategy=local)
      local a b sa sb r
      a=$(run_sorter "$STAGE/base_scatter_sort" "d1_base_${arm}_$name" "$f" \
          --threads 4 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
          --scratch-budget-bytes 20000000000 "${x[@]+"${x[@]}"}")
      b=$(run_sorter "$STAGE/scatter_sort" "d1_new_${arm}_$name" "$f" \
          --threads 4 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
          --scratch-budget-bytes 20000000000 "${x[@]+"${x[@]}"}")
      sa=$(sha_of "$a"); sb=$(sha_of "$b")
      r=$(stat -c %s "$b/out.sa")
      [ "$sa" = "$sb" ] || fail "D1 $name/$arm band streams differ ($sa vs $sb)"
      "$STAGE/sa_check" --corpus "$f" --sa "$b/out.sa" --dollar 1 > "$LOG/d1_sacheck_${arm}_$name.log" 2>&1 \
        || fail "sa_check $name/$arm"
      local left; left=$(posdir_of "$b")
      [ "$left" = 0 ] || fail "D1 $name/$arm left $left position files behind"
      echo "SUBDIV_DISK_D1 fixture=$name arm=$arm rows=$(( (r - 48) / 8 )) sha=$sb windows=$(wins_of "$b") peak_total=$(peak_of "$b") position_files_left=0 sa_check=PASS"
      [ "$arm" = auto ] && { n=$((n+1)); rows=$((rows + (r - 48) / 8)); }
    done
  done
  echo "SUBDIV_DISK_D1_PASS fixtures=$n arms=2 rows=$rows (band stream byte-identical, sa_check PASS, subdiv_pos empty)"
}

# COMMON flags for the corpus-scale arms: identical for every arm (same plan shape).
# STRATEGY=local forces the local arm, the only arm that uses a position window.
# It matters at small scale: at 10 GB the plan's own choice is ALL-GLOBAL
# (page_fraction ~ 1 for every class), so an auto run never creates a window and a
# peak gate over it would be vacuous - the vacuous pass this project calls a bug.
# Every window gate therefore runs BOTH arms.
STRATEGY=${STRATEGY:-auto}
corpus_args() { echo --threads "$THREADS" --budget-bytes "$BUDGET" --memory-gb "$MEM_GB" \
                    --scratch-budget-bytes "$((SCRATCH_GB * 1000000000))" \
                    --subdiv-strategy "$STRATEGY"; }

# P6: a clean exit, checked on every corpus-scale run: the sorter's own done line,
# nothing left in subdiv_pos, and (with tape bands) the producer's done marker.
clean_check() {
  local d=$1 tag=$2
  grep -q '^SS_DONE' "$d/run.log" || fail "P6 $tag: no SS_DONE line (the run did not finish cleanly)"
  local left bytes
  left=$(posdir_of "$d"); bytes=$(posbytes_of "$d")
  [ "$left" = 0 ] && [ "$bytes" = 0 ] || fail "P6 $tag: subdiv_pos holds $left files / $bytes bytes at exit"
  local done_line scratch_bytes
  done_line=$(grep -o '^SS_DONE .*' "$d/run.log" | tail -1)
  scratch_bytes=$(du -sb "$d/scratch" 2>/dev/null | cut -f1)
  echo "SUBDIV_DISK_P6 tag=$tag position_files_left=0 position_bytes_left=0 scratch_bytes_after_exit=${scratch_bytes:-0} $done_line"
}

# --------------------------------------------------------------------------- D2
d2() {
  echo "== D2 peak bounded: corpus=$CORPUS rows=$(stat -c %s "$CORPUS") pos_budget=$PEAK_BUDGET"
  local a b
  a=$(run_sorter "$STAGE/base_scatter_sort" d2_base "$CORPUS" $(corpus_args) --subdiv-position-budget-bytes "$PEAK_BUDGET")
  b=$(run_sorter "$STAGE/scatter_sort" d2_new "$CORPUS" $(corpus_args) --subdiv-position-budget-bytes "$PEAK_BUDGET")
  local peak wins passes wall left bytes
  peak=$(peak_of "$b"); wins=$(wins_of "$b"); passes=$(passes_of "$b")
  wall=$(cat "$b/wall_s"); left=$(posdir_of "$b"); bytes=$(posbytes_of "$b")
  local budget_plus_slack=$(( PEAK_BUDGET + PEAK_BUDGET / 20 ))
  echo "SUBDIV_DISK_D2 peak_total=$peak window_slack_ceiling=$budget_plus_slack windows=$wins passes=$passes wall_s=$wall position_files_left=$left position_bytes_left=$bytes pos_budget=$PEAK_BUDGET"
  peak_wins "$b" | sed 's/^/SUBDIV_DISK_D2_POSPEAK /'
  [ -n "$peak" ] || fail "D2 no SS_SUBDIV_POSPEAK line: no window was ever materialized"
  [ "$peak" -le "$budget_plus_slack" ] || fail "D2 peak $peak > $budget_plus_slack"
  [ "$left" = 0 ] && [ "$bytes" = 0 ] || fail "D2 subdiv_pos not drained ($left files, $bytes bytes)"
  [ -f "$b/out.sa" ] || fail "D2 no output"
  local lrows proj waves_j want
  lrows=$(grep -o 'local_rows_est=[0-9]*' "$b/run.log" | head -1 | cut -d= -f2)
  proj=$(grep -o 'projected_passes=[0-9]*' "$b/run.log" | head -1 | cut -d= -f2)
  waves_j=$(grep -o 'local_waves=[0-9]*' "$b/run.log" | head -1 | cut -d= -f2)
  want=$(( (lrows * 8 + PEAK_BUDGET - 1) / PEAK_BUDGET ))
  echo "SUBDIV_DISK_D2_SWEEPS local_rows_measured=$lrows local_waves=$waves_j pos_budget=$PEAK_BUDGET ceil_local_rows_x8_over_budget=$want projected_passes_journaled=$proj measured_scatter_sweeps=$passes windows=$wins"
  [ "$wins" -gt 0 ] || fail "D2 the plan produced no local wave: no window to measure (set STRATEGY=local)"
  clean_check "$b" d2_new
  echo "SUBDIV_DISK_D2_PASS (peak is the budget, not the corpus; subdiv_pos empty at exit)"
}

# --------------------------------------------------------------------------- D3
d3() {
  echo "== D3 same order: this build vs a fresh pre-fix build, same flags, corpus=$CORPUS, arms=auto+local"
  local arm
  for arm in auto local; do
    STRATEGY=$arm
    local a b sa sb lw sgw wins
    a=$(run_sorter "$STAGE/base_scatter_sort" "d3_base_$arm" "$CORPUS" $(corpus_args) --subdiv-position-budget-bytes "$PEAK_BUDGET")
    b=$(run_sorter "$STAGE/scatter_sort" "d3_new_$arm" "$CORPUS" $(corpus_args) --subdiv-position-budget-bytes "$PEAK_BUDGET")
    sa=$(sha_of "$a"); sb=$(sha_of "$b")
    lw=$(grep -o 'local_waves=[0-9]*' "$b/run.log" | head -1)
    sgw=$(grep -o 'corpus_sweeps_scatter=[0-9]*' "$b/run.log" | head -1)
    wins=$(wins_of "$b")
    echo "SUBDIV_DISK_D3 arm=$arm base_sha=$sa new_sha=$sb equal=$([ "$sa" = "$sb" ] && echo 1 || echo 0) rows=$(( ($(stat -c %s "$b/out.sa") - 48) / 8 )) base_wall_s=$(cat "$a/wall_s") new_wall_s=$(cat "$b/wall_s") $lw $sgw windows=$wins"
    [ "$sa" = "$sb" ] || fail "D3 containers differ ($arm)"
    if [ "$arm" = local ]; then
      [ "$wins" -gt 0 ] || fail "D3 the forced-local arm created no position window: the gate would be vacuous"
      "$STAGE/sa_check" --corpus "$CORPUS" --sa "$b/out.sa" --dollar 1 > "$LOG/d3_sacheck.log" 2>&1 || fail "D3 sa_check"
      echo "$sb" > "$LOG/d3_local_sha"
      clean_check "$b" d3_new_local
    fi
  done
  echo "SUBDIV_DISK_D3_PASS"
}

# --------------------------------------------------------------------------- D4
d4() {
  echo "== D4 budget-bounded peak: same corpus, pos_budget=$TIGHT_BUDGET (vs $PEAK_BUDGET for D2)"
  STRATEGY=local
  local b sa_ref
  sa_ref=$(cat "$LOG/d3_local_sha" 2>/dev/null || sha_of "$WORK/runs/d3_new_local")
  b=$(run_sorter "$STAGE/scatter_sort" d4_tight "$CORPUS" $(corpus_args) --subdiv-position-budget-bytes "$TIGHT_BUDGET")
  local peak wins passes wall left sb ceiling
  peak=$(peak_of "$b"); wins=$(wins_of "$b"); passes=$(passes_of "$b"); wall=$(cat "$b/wall_s")
  left=$(posdir_of "$b"); sb=$(sha_of "$b"); ceiling=$(( TIGHT_BUDGET + TIGHT_BUDGET / 20 ))
  echo "SUBDIV_DISK_D4 peak_total=$peak ceiling=$ceiling windows=$wins passes=$passes wall_s=$wall container_sha=$sb reference_sha=$sa_ref equal=$([ "$sb" = "$sa_ref" ] && echo 1 || echo 0) position_files_left=$left"
  peak_wins "$b" | sed 's/^/SUBDIV_DISK_D4_POSPEAK /'
  [ "$sb" = "$sa_ref" ] || fail "D4 container differs from D3's"
  [ "$peak" -le "$ceiling" ] || fail "D4 peak $peak > $ceiling"
  [ "$left" = 0 ] || fail "D4 subdiv_pos not drained"
  clean_check "$b" d4_tight
  echo "SUBDIV_DISK_D4_PASS (the budget bounds the peak; the bytes are unchanged)"
}

# --------------------------------------------------------------------------- D5
# THE REFUSAL: the scratch filesystem is a real filesystem with less free space than
# the window needs. Anything the operator can point --scratch at works; the gate only
# needs `D5_SCRATCH` to exist and to have less free than (needed + the 50 GB margin).
d5() {
  echo "== D5 refusal: a scratch filesystem that cannot hold the window"
  local small=${D5_SCRATCH:-}
  if [ -z "$small" ]; then
    # /run/user/<uid> is a small tmpfs on this box; any filesystem with less free
    # bytes than the window needs + the margin works.
    small=/run/user/$(id -u)
    mkdir -p "$small/sxgc-d5" 2>/dev/null || fail "D5: no small scratch filesystem (set D5_SCRATCH)"
    small=$small/sxgc-d5
  fi
  local need_free
  need_free=$(df -B1 --output=avail "$small" | tail -1 | tr -d ' ')
  echo "SUBDIV_DISK_D5 scratch=$small free_bytes=$need_free"
  local d=$WORK/runs/d5_refuse
  echo d5_refuse >> "$RUN_TAGFILE"
  rm -rf "$d"; mkdir -p "$d/scratch"
  local rc=0
  setsid timeout --kill-after=10 "$RUN_TIMEOUT" "$STAGE/scatter_sort" \
      --corpus "$CORPUS" --out "$d/out.sa" --scratch "$small" $(corpus_args) \
      --subdiv-position-budget-bytes 2000000000000 > "$d/run.log" 2>&1 || rc=$?
  # The run must FAIL, and it must fail with the numbers in the message.
  local line
  line=$(grep -o 'subdiv position window does not fit the scratch filesystem[^"]*' "$d/run.log" | head -1 || true)
  [ "$rc" -ne 0 ] || fail "D5 the run completed on a filesystem too small for its window"
  [ -n "$line" ] || { tail -20 "$d/run.log" >&2; fail "D5 died without the guard's numbers"; }
  local wrote; wrote=$(find "$small" -type f 2>/dev/null | wc -l)
  echo "SUBDIV_DISK_D5 rc=$rc files_written_before_refusal=$wrote"
  echo "SUBDIV_DISK_D5_MESSAGE $line"
  echo "SUBDIV_DISK_D5_PASS (refused with free/needed/margin instead of filling the disc)"
}

# ------------------------------------------------------------------------- verify
# Re-derive a corpus-scale verdict from an EXISTING run directory (no re-run): the
# journal is the evidence, so a gate can be re-checked without spending the wall
# clock again. usage: verify <run-dir> <reference-sha> <pos_budget>
verify() {
  local d=$1 ref=$2 budget=$3
  local peak wins passes left bytes sb done_line sweeps
  peak=$(peak_of "$d"); wins=$(wins_of "$d"); passes=$(passes_of "$d")
  left=$(posdir_of "$d"); bytes=$(posbytes_of "$d"); sb=$(sha_of "$d")
  done_line=$(grep -o '^SS_DONE .*' "$d/run.log" | tail -1)
  sweeps=$(grep -o 'SS_SUBDIV_SWEEPS .*' "$d/run.log" | tail -1 | sed 's/ rule=.*//')
  echo "VERIFY dir=$d pos_budget=$budget peak_total=$peak budget_ceiling=$(( budget + budget / 20 )) windows=$wins scatter_passes=$passes position_files_left=$left position_bytes_left=$bytes container_sha=$sb reference_sha=$ref equal=$([ "$sb" = "$ref" ] && echo 1 || echo 0)"
  echo "VERIFY_SWEEPS $sweeps"
  echo "VERIFY_DONE $done_line"
  [ -n "$done_line" ] || fail "verify: no SS_DONE"
  [ "$sb" = "$ref" ] || fail "verify: container differs"
  [ "$peak" -le $(( budget + budget / 20 )) ] || fail "verify: peak above the budget"
  [ "$left" = 0 ] && [ "$bytes" = 0 ] || fail "verify: subdiv_pos not drained"
  [ "$wins" = "$passes" ] || fail "verify: windows != scatter passes"
  echo "VERIFY_PASS"
}

case "${1:-all}" in
  verify) verify "$2" "$3" "$4" ;;
  d1) build; fixtures; d1; no_survivors ;;
  d2) build; d2; no_survivors ;;
  d3) build; d3; no_survivors ;;
  d4) build; d4; no_survivors ;;
  d5) build; d5; no_survivors ;;
  all) build; fixtures; d1; d2; d3; d4; d5; no_survivors ;;
  *) echo "usage: $0 d1|d2|d3|d4|d5|all" >&2; exit 2 ;;
esac
