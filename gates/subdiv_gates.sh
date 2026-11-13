#!/usr/bin/env bash
# subdiv_gates.sh - the nine gates of WAVE_SUBDIVISION.md.
#
#   gates/subdiv_gates.sh g1|g2|g3|g4|g5|g6|g7|g8|g9|all
#
# Environment:
#   WORK=<scratch root>            default /mnt/nvme2n1/erikg/SUBDIV-GATES
#   BASE_REF=<git rev of the borrow arm>  default 6cf19f2 (the spec commit)
#   TEN_GB=<10 GB corpus>          default /mnt/nvme2n1/erikg/pf-10gb.txt
#   HUNDRED_GB=<100 GB head>       default /mnt/nvme2n1/erikg/pile-head100.txt
#   THREADS / BUDGET_GB / SCRATCH_GB for the large-corpus gates
#   FIXBYTES=<fixture size>        default 6291456 (the sorter's band floor needs
#                                  >= ~4.4 MB at --par-bands 1)
#   SUBDIV_KEY=<the class G3 measures>   default "Harper Reed"
#
# Every product run is wrapped in `setsid timeout --kill-after=10` and the script
# asserts that no stage process survives it.
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/mnt/nvme2n1/erikg/SUBDIV-GATES}
BASE_REF=${BASE_REF:-6cf19f2}
TEN_GB=${TEN_GB:-/mnt/nvme2n1/erikg/pf-10gb.txt}
HUNDRED_GB=${HUNDRED_GB:-/mnt/nvme2n1/erikg/pile-head100.txt}
THREADS=${THREADS:-96}
BUDGET_GB=${BUDGET_GB:-96}
SCRATCH_GB=${SCRATCH_GB:-250}
FIXBYTES=${FIXBYTES:-6291456}
SUBDIV_KEY=${SUBDIV_KEY:-Harper Reed}
STAGE=$WORK/stage
FX=$WORK/fixtures
LOG=$WORK/logs
mkdir -p "$STAGE" "$FX" "$LOG"

CXX=${CXX:-g++}
CFLAGS="-O2 -std=c++17 -pthread"

fail() { echo "SUBDIV_GATE_FAIL: $*" >&2; exit 3; }
no_survivors() {
  local pids
  pids=$(pgrep -x scatter_sort || true)
  [ -z "$pids" ] || { echo "SUBDIV_SURVIVORS: $pids"; fail "a stage process survived"; }
  echo "SUBDIV_NO_SURVIVORS checked=scatter_sort"
}

build() {
  echo "== BUILD stage=$STAGE"
  $CXX $CFLAGS "$HERE/xsa/runtime/tools/binbwt/scatter_sort.cpp" -o "$STAGE/scatter_sort" -latomic
  $CXX $CFLAGS "$HERE/xsa/runtime/tools/binbwt/sa_check.cpp"      -o "$STAGE/sa_check"
  $CXX $CFLAGS "$HERE/gates/subdiv_oracle.cpp"                   -o "$STAGE/subdiv_oracle"
  $CXX $CFLAGS "$HERE/gates/subdiv_fixtures.c"                   -o "$STAGE/subdiv_fixtures"
  echo "SUBDIV_BUILD scatter_sort_sha=$(sha256sum "$STAGE/scatter_sort" | cut -d' ' -f1)"
  if [ ! -x "$STAGE/base_scatter_sort" ] || [ "${REBUILD_BASE:-0}" = 1 ]; then
    git -C "$HERE" show "$BASE_REF:xsa/runtime/tools/binbwt/scatter_sort.cpp" > "$STAGE/base_scatter_sort.cpp"
    $CXX $CFLAGS "$STAGE/base_scatter_sort.cpp" -o "$STAGE/base_scatter_sort" -latomic
  fi
  echo "SUBDIV_BASE ref=$BASE_REF sha=$(sha256sum "$STAGE/base_scatter_sort" | cut -d' ' -f1)"
}

fixtures() {
  if [ ! -s "$FX/docsep.txt" ] || [ "$(stat -c %s "$FX/docsep.txt")" != "$FIXBYTES" ]; then
    rm -f "$FX"/*.txt
    "$STAGE/subdiv_fixtures" "$FX" "$FIXBYTES" > "$LOG/fixtures.log"
  fi
  echo "SUBDIV_FIXTURES dir=$FX bytes=$FIXBYTES n=$(ls "$FX"/*.txt | wc -l) names=$(cd "$FX" && ls *.txt | tr '\n' ' ')"
}

# run_sorter BIN TAG CORPUS EXTRA... - one product run, no survivors, log kept.
run_sorter() {
  local bin=$1 tag=$2 corpus=$3; shift 3
  local d=$WORK/runs/$tag
  rm -rf "$d"; mkdir -p "$d/scratch"
  echo "SUBDIV_RUN tag=$tag corpus=$corpus bytes=$(stat -c %s "$corpus") extra='$*'" >&2
  setsid timeout --kill-after=10 "${RUN_TIMEOUT:-7200}" "$bin" \
      --corpus "$corpus" --out "$d/out.sa" --scratch "$d/scratch" "$@" \
      > "$d/run.log" 2>&1 || { echo "rc=$?"; tail -5 "$d/run.log" >&2; fail "run $tag failed"; }
  echo "$d"
}

# THE BAND STREAM's identity: the tape's payload, i.e. the bands concatenated in
# rank order. That is exactly what --out holds (the emitted rank stream), so its
# sha256 IS the band-stream sha; the per-band row VECTOR is reported beside it
# because two correct band plans may cut the same stream at different rows (a row
# whose key equals a fence key is order-safe on either side of the cut) - and a
# gate that called that a difference would be measuring the plan, not the stream.
band_stream_sha() { sha256sum "$1/out.sa" | cut -d' ' -f1; }
band_rows_sha()   { grep -o 'SS_BAND band=[0-9]* rows=[0-9]*' "$1/run.log" | \
                    sed 's/SS_BAND band=//; s/ rows=/ /' | sort -n -k1,1 | sha256sum | cut -d' ' -f1; }
band_rows_diff()  {
  diff <(grep -o 'SS_BAND band=[0-9]* rows=[0-9]*' "$1/run.log" | sed 's/SS_BAND band=//; s/ rows=/ /' | sort -n -k1,1) \
       <(grep -o 'SS_BAND band=[0-9]* rows=[0-9]*' "$2/run.log" | sed 's/SS_BAND band=//; s/ rows=/ /' | sort -n -k1,1) \
    | grep -c '^<' || true
}

# --------------------------------------------------------------------------- G1
g1() {
  echo "== G1 fixture parity (borrow arm vs this path), N fixtures"
  local n=0 rows=0
  for f in "$FX"/*.txt; do
    local name; name=$(basename "$f" .txt)
    local a b
    a=$(run_sorter "$STAGE/base_scatter_sort" "g1_base_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
        --scratch-budget-bytes 20000000000)
    b=$(run_sorter "$STAGE/scatter_sort" "g1_new_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
        --scratch-budget-bytes 20000000000)
    local sa sb r bands diffn
    sa=$(band_stream_sha "$a"); sb=$(band_stream_sha "$b")
    bands=$(grep -c TAPE_ROW_BOUND "$b/run.log")
    diffn=$(band_rows_diff "$a" "$b")
    r=$(stat -c %s "$b/out.sa")
    "$STAGE/sa_check" --corpus "$f" --sa "$b/out.sa" --dollar 1 > "$LOG/g1_sacheck_$name.log" 2>&1 \
      || fail "sa_check $name"
    echo "SUBDIV_G1 fixture=$name rows=$(( (r - 48) / 8 )) bands=$bands base_sha=$sa new_sha=$sb equal=$([ "$sa" = "$sb" ] && echo 1 || echo 0) band_vectors_differ_in=$diffn sa_check=$(grep -o 'SA_CHECK_PASS' "$LOG/g1_sacheck_$name.log" | head -1)"
    [ "$sa" = "$sb" ] || fail "G1 $name band streams differ"
    n=$((n+1)); rows=$((rows + (r - 48) / 8))
  done
  echo "SUBDIV_G1_PASS fixtures=$n rows=$rows (band stream byte-identical, sa_check PASS on every one)"
}

# --------------------------------------------------------------------------- G2
g2() {
  echo "== G2 brute-force SA oracle, N cases"
  local n=0
  for f in "$FX"/*.txt; do
    local name; name=$(basename "$f" .txt)
    local d; d=$(run_sorter "$STAGE/scatter_sort" "g2_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
        --scratch-budget-bytes 20000000000)
    "$STAGE/subdiv_oracle" --corpus "$f" --sa "$d/out.sa" | tee "$LOG/g2_$name.log" | grep SUBDIV_ORACLE
    grep -q SUBDIV_ORACLE_PASS "$LOG/g2_$name.log" || fail "G2 $name oracle disagrees"
    n=$((n+1))
  done
  echo "SUBDIV_G2_PASS cases=$n (EOF run, doc-boundary 0x1E, internal 0x1E, r=0 tails, mixed families, periodic run, single-symbol run)"
}

# --------------------------------------------------------------------------- G3
g3() {
  echo "== G3 read accounting: no global rescan after the scatter, local bytes independent of n"
  echo "   class='$SUBDIV_KEY' at 10 GB and at the 100 GB head"
  local d10 d100
  d10=$(run_sorter "$STAGE/scatter_sort" g3_10gb "$TEN_GB" \
      --threads "$THREADS" --budget-bytes "$((BUDGET_GB*1000000000))" --memory-gb "$BUDGET_GB" \
      --scratch-budget-bytes "$((SCRATCH_GB*1000000000))" --subdiv-class="$SUBDIV_KEY" \
      --subdiv-position-budget-bytes=20000000000)
  d100=$(run_sorter "$STAGE/scatter_sort" g3_100gb "$HUNDRED_GB" \
      --threads "$THREADS" --budget-bytes "$((BUDGET_GB*1000000000))" --memory-gb "$BUDGET_GB" \
      --scratch-budget-bytes "$((SCRATCH_GB*1000000000))" --subdiv-class="$SUBDIV_KEY" \
      --subdiv-position-budget-bytes=20000000000)
  for d in "$d10" "$d100"; do
    grep -E "SS_PLAN |SS_SCATTER |SS_SUBDIV_CLASS_ONLY |SS_SUBDIV_STRATEGY|SS_SUBDIV_EXACT" "$d/run.log" | head -8
    grep -q "corpus_reads=0 " "$d/run.log" || fail "G3: PLAN touched the corpus in $d"
    # local bytes_read must be the class's OWN page footprint, not n: the exact
    # page count comes from the descent's own position walk.
    local bytes pages n
    bytes=$(grep -o 'SS_SUBDIV_STRATEGY .*bytes_read=[0-9]*' "$d/run.log" | grep -o 'bytes_read=[0-9]*' | cut -d= -f2 | head -1)
    pages=$(grep -o 'SS_SUBDIV_EXACT .*pages_exact=[0-9]*' "$d/run.log" | grep -o 'pages_exact=[0-9]*' | cut -d= -f2 | head -1)
    n=$(grep -o '^SS_START n=[0-9]*' "$d/run.log" | cut -d= -f2)
    [ -n "$bytes" ] && [ -n "$pages" ] || fail "G3: missing counters in $d"
    local bound=$(( (pages + 2) * 1048576 ))
    echo "SUBDIV_G3_SCALE dir=$d n=$n class_bytes_read=$bytes pages_exact=$pages bound=$((pages+2))MiB within=$([ "$bytes" -le "$bound" ] && echo 1 || echo 0) n_fraction=$(awk -v b="$bytes" -v n="$n" 'BEGIN{printf "%.6f", b/n}')"
    [ "$bytes" -le "$bound" ] || fail "G3: local bytes_read exceeded the class's own page footprint in $d"
  done
  # the global arm is exactly ONE sweep per class, never one per level
  local dg; dg=$(run_sorter "$STAGE/scatter_sort" g3_forced_global "$TEN_GB" \
      --threads "$THREADS" --budget-bytes "$((BUDGET_GB*1000000000))" --memory-gb "$BUDGET_GB" \
      --scratch-budget-bytes "$((SCRATCH_GB*1000000000))" --subdiv-class="$SUBDIV_KEY" \
      --subdiv-strategy=global --subdiv-position-budget-bytes=20000000000)
  local slices classes
  slices=$(grep -c 'passes=1' "$dg/run.log" || true)
  classes=$(grep -c 'SS_SUBDIV_STRATEGY' "$dg/run.log" || true)
  echo "SUBDIV_G3_GLOBAL dir=$dg classes=$classes sweeps=$slices (each class exactly one)"
  [ "$slices" = "$classes" ] || fail "G3: the forced-global arm did not do exactly one sweep per class"
  echo "SUBDIV_G3_PASS (PLAN corpus_reads=0; local bytes_read <= the class's own page footprint at BOTH scales)"
  grep -o 'SS_SCATTER pass=[0-9]* [^r]*' "$d100/run.log" | head -3
}

# --------------------------------------------------------------------------- G4
g4() {
  echo "== G4 the 100 GB head's space class through both arms"
  local dl dg
  dl=$(run_sorter "$STAGE/scatter_sort" g4_100gb_local "$HUNDRED_GB" \
      --threads "$THREADS" --budget-bytes "$((BUDGET_GB*1000000000))" --memory-gb "$BUDGET_GB" \
      --scratch-budget-bytes "$((SCRATCH_GB*1000000000))" --subdiv-class=' ' --subdiv-strategy=local \
      --subdiv-position-budget-bytes=40000000000)
  dg=$(run_sorter "$STAGE/scatter_sort" g4_100gb_global "$HUNDRED_GB" \
      --threads "$THREADS" --budget-bytes "$((BUDGET_GB*1000000000))" --memory-gb "$BUDGET_GB" \
      --scratch-budget-bytes "$((SCRATCH_GB*1000000000))" --subdiv-class=' ' --subdiv-strategy=global \
      --subdiv-position-budget-bytes=40000000000)
  local sl sg rows wl wg
  sl=$(band_stream_sha "$dl"); sg=$(band_stream_sha "$dg")
  rows=$(grep -o 'SS_SUBDIV_CLASS_ONLY .*rows_est=[0-9]*' "$dl/run.log" | grep -o 'rows_est=[0-9]*' | cut -d= -f2 | head -1)
  wl=$(grep -o 'SS_DONE .*total_s=[0-9.]*' "$dl/run.log" | tail -1)
  wg=$(grep -o 'SS_DONE .*total_s=[0-9.]*' "$dg/run.log" | tail -1)
  grep -E "SS_SUBDIV_CLASS_ONLY |SS_SUBDIV_STRATEGY|SS_SCATTER |SS_DONE" "$dl/run.log" | head -6
  grep -E "SS_SUBDIV_CLASS_ONLY |SS_SUBDIV_STRATEGY|SS_SCATTER |SS_DONE" "$dg/run.log" | head -6
  echo "SUBDIV_G4 class_rows_est=$rows local_sha=$sl global_sha=$sg equal=$([ "$sl" = "$sg" ] && echo 1 || echo 0)"
  echo "SUBDIV_G4_WALL local: $wl | global: $wg"
  [ "$sl" = "$sg" ] || fail "G4: the two arms produced different band streams at 100 GB"
  echo "SUBDIV_G4_PASS"
}

# --------------------------------------------------------------------------- G5
g5() {
  echo "== G5 refusal: a class the census cannot prove still refuses, with numbers, and never suggests a merge"
  local d=$WORK/runs/g5; rm -rf "$d"; mkdir -p "$d/scratch"
  set +e
  setsid timeout --kill-after=10 600 "$STAGE/scatter_sort" --corpus "$FX/refuse.txt" \
      --out "$d/out.sa" --scratch "$d/scratch" --threads 2 --par-bands 1 \
      --budget-bytes 2000000000 --memory-gb 0.01 --scratch-budget-bytes 20000000000 \
      > "$d/run.log" 2>&1
  local rc=$?
  set -e
  no_survivors
  grep -E "SS_WAVE_POINT_MASS_PROOF|SS_WAVE_POINT_MASS depth|SS_WAVE_BORROW|SS_SUBDIV_COVERAGE_STALL" "$d/run.log" | head -4
  grep -E "scatter_sort: SS_WAVE_POINT_MASS" "$d/run.log" | head -2
  echo "SUBDIV_G5 rc=$rc refused=$([ $rc -eq 2 ] && echo 1 || echo 0)"
  [ "$rc" -eq 2 ] || fail "G5: the unprovable class did NOT refuse"
  grep -q "NO MERGE is a path here" "$d/run.log" || fail "G5: the refusal does not say no merge"
  if grep -q "implement SS_WAVE_CHUNKMERGE" "$d/run.log"; then fail "G5: the refusal still suggests a merge"; fi
  grep -q "class_rows=" "$d/run.log" || fail "G5: the refusal carries no numbers"
  echo "SUBDIV_G5_PASS (numbers above; levers are --memory-gb and a wider prefix cache; a merge is never a path)"
}

# --------------------------------------------------------------------------- G6
g6() {
  echo "== G6 strategy equivalence: forced local vs forced global, N fixtures"
  local n=0
  for name in docsep internsep mixed eofrun r0; do
    local f=$FX/$name.txt
    local dl dg
    dl=$(run_sorter "$STAGE/scatter_sort" "g6_local_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
        --scratch-budget-bytes 20000000000 --subdiv-strategy=local)
    dg=$(run_sorter "$STAGE/scatter_sort" "g6_global_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
        --scratch-budget-bytes 20000000000 --subdiv-strategy=global)
    local sl sg
    sl=$(band_stream_sha "$dl"); sg=$(band_stream_sha "$dg")
    # the coverage estimate against the exact count from the class's full list
    local worst
    worst=$(grep -o 'coverage_exact=[0-9.]* coverage_est=[0-9.]*' "$dl/run.log" | \
      awk '{split($1,a,"="); split($2,b,"="); k=(a[2]>b[2])?a[2]:b[2];
            if (k>0) { e=(a[2]-b[2])/k; if (e<0) e=-e; if (e>m) m=e; n++ } }
            END { printf "%.6f classes=%d", (n?m:0), n }')
    echo "SUBDIV_G6 fixture=$name local_sha=$sl global_sha=$sg equal=$([ "$sl" = "$sg" ] && echo 1 || echo 0) coverage_est_vs_exact_max_rel_err=$worst"
    [ "$sl" = "$sg" ] || fail "G6: $name local and global disagree"
    n=$((n+1))
  done
  echo "SUBDIV_G6_PASS fixtures=$n (byte-identical band streams under forced local and forced global)"
}

# --------------------------------------------------------------------------- G7
g7() {
  echo "== G7 survey reproducibility: two surveys, identical lines and blob sha"
  local n=0
  for name in docsep mixed; do
    local f=$FX/$name.txt
    local a b
    a=$(run_sorter "$STAGE/scatter_sort" "g7_a_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 --scratch-budget-bytes 20000000000)
    b=$(run_sorter "$STAGE/scatter_sort" "g7_b_$name" "$f" \
        --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 --scratch-budget-bytes 20000000000)
    local la lb
    la=$(grep '^SS_SURVEY ' "$a/run.log"); lb=$(grep '^SS_SURVEY ' "$b/run.log")
    echo "SUBDIV_G7 fixture=$name line_a='$la'"
    echo "SUBDIV_G7 fixture=$name line_b='$lb'"
    [ "$la" = "$lb" ] || fail "G7: the two SS_SURVEY lines differ for $name"
    # the same survey, twice, in ONE corpus: a second run of the same binary on the
    # same bytes must reproduce the sha exactly (above); and the sha must change if
    # the corpus changes.
    n=$((n+1))
  done
  local shas
  shas=$(grep -h '^SS_SURVEY ' "$WORK"/runs/g7_*_*/run.log | grep -o 'sha256=[0-9a-f]*' | sort -u | wc -l)
  echo "SUBDIV_G7 distinct_shas_across_the_two_fixtures=$shas (2 = the sha tracks the corpus, so it is not a constant)"
  [ "$shas" -eq 2 ] || fail "G7: the survey sha does not distinguish the two corpora"
  echo "SUBDIV_G7_PASS surveys=$n"
}

# --------------------------------------------------------------------------- G8
g8() {
  echo "== G8 plan == reality: predicted rows/coverage vs the class's full position list"
  # THE SCALE MATTERS: on a fixture whose stride is 1 the sample IS the corpus and
  # the comparison is vacuous. G8 therefore runs on a slice large enough that the
  # stride is a real stride (default 512 MB from the 10 GB corpus -> stride ~119).
  local slice=$WORK/slices/g8slice.txt
  local want=${G8_BYTES:-536870912}
  mkdir -p "$WORK/slices"
  if [ ! -s "$slice" ] || [ "$(stat -c %s "$slice")" != "$want" ]; then
    head -c "$want" "$TEN_GB" > "$slice" || fail "G8: cannot cut the slice"
  fi
  local d; d=$(run_sorter "$STAGE/scatter_sort" g8 "$slice" \
      --threads 8 --par-bands 2 --budget-bytes 8000000000 --memory-gb 8 \
      --scratch-budget-bytes 60000000000)
  grep -o 'SS_SUBDIV_EXACT .*' "$d/run.log" | head -3
  awk '/SS_SUBDIV_EXACT/ {
         for (i = 1; i <= NF; ++i) {
           if ($i ~ /^rows_err=/)   { split($i, a, "="); e = a[2]; if (e < 0) e = -e;
                                      n++; s += e; if (e > mx) mx = e;
                                      if (e <= 0.01) b1++; else if (e <= 0.05) b2++; else b3++ }
           if ($i ~ /^coverage_exact=/) { split($i, a, "="); ce = a[2] + 0 }
           if ($i ~ /^coverage_est=/)   { split($i, a, "="); cg = a[2] + 0;
                                      k = (ce > cg) ? ce : cg; if (k > 0) { q = (ce - cg) / k; if (q < 0) q = -q;
                                      cn++; if (q > cmax) cmax = q } }
         }
       }
       END { printf "SUBDIV_G8 classes=%d rows_err_mean=%.5f rows_err_max=%.5f within_1pct=%d within_5pct=%d worse_than_5pct=%d coverage_classes=%d coverage_max_rel_err=%.4f\n",
                    n, (n ? s/n : 0), mx, b1, b2, b3, cn, cmax }' "$d/run.log"
  local mx
  mx=$(awk '/SS_SUBDIV_EXACT/ { for (i=1;i<=NF;++i) if ($i ~ /^rows_err=/) { split($i,a,"="); e=a[2]; if (e<0) e=-e; if (e>m) m=e } } END { print m+0 }' "$d/run.log")
  echo "SUBDIV_G8 max_rows_err=$mx"
  awk -v m="$mx" 'BEGIN { exit (m > 0.05) ? 1 : 0 }' || fail "G8: a class's predicted rows were off by more than 5%"
  echo "SUBDIV_G8_PASS (the error distribution above is over every wave of the fixture)"
}

# --------------------------------------------------------------------------- G9
g9() {
  echo "== G9 no census reads: PLAN corpus reads == 0, DESCENT reads are window reads"
  local d; d=$(run_sorter "$STAGE/scatter_sort" g9 "$FX/internsep.txt" \
      --threads 2 --par-bands 1 --budget-bytes 2000000000 --memory-gb 2 \
      --scratch-budget-bytes 20000000000)
  local line
  line=$(grep '^SS_PLAN ' "$d/run.log")
  echo "SUBDIV_G9 plan: $line"
  echo "$line" | grep -q 'corpus_reads=0 ' || fail "G9: PLAN read the corpus"
  echo "$line" | grep -q 'cache_misses=0 ' || fail "G9: the plan had a cache miss on a fixture whose depths fit the cache"
  # during the descent, a local class reads only pages: bytes_read <= (pages+2) MiB
  awk '/SS_SUBDIV_EXACT/ {
         bytes = 0; pages = 0;
         for (i = 1; i <= NF; ++i) {
           if ($i ~ /^bytes_read=/)   { split($i, a, "="); bytes = a[2] + 0 }
           if ($i ~ /^pages_exact=/)  { split($i, a, "="); pages = a[2] + 0 }
         }
         n++; tot += bytes; ptot += pages;
         if (bytes > (pages + 2) * 1048576) bad++
       }
       END { printf "SUBDIV_G9 descent classes=%d bytes_read_total=%d pages_exact_total=%d over_page_bound=%d\n", n, tot, ptot, bad+0;
             exit (bad + 0) ? 1 : 0 }' "$d/run.log" || fail "G9: a local class read more than its own pages"
  echo "SUBDIV_G9_PASS (PLAN corpus_reads=0 cache_misses=0; every local descent stayed inside its page bound)"
}

case ${1:-all} in
  g1) build; fixtures; g1; no_survivors ;;
  g2) build; fixtures; g2 ;;
  g3) build; g3; no_survivors ;;
  g4) build; g4; no_survivors ;;
  g5) build; fixtures; g5 ;;
  g6) build; fixtures; g6; no_survivors ;;
  g7) build; fixtures; g7; no_survivors ;;
  g8) build; fixtures; g8 ;;
  g9) build; fixtures; g9 ;;
  all) build; fixtures; g1; g2; g6; g7; g8; g9; g5; g3; g4; no_survivors ;;
  *) fail "unknown gate '$1' (g1..g9|all)" ;;
esac
echo "SUBDIV_GATE_DONE gate=${1:-all}"
