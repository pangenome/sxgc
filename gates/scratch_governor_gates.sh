#!/usr/bin/env bash
# scratch_governor_gates.sh - the five gates of RESULTS_SCRATCH_GOVERNOR.md.
#
#   gates/scratch_governor_gates.sh s1|s2|s3|s4|s6|all
#
# S1 TERM COVERAGE   the journaled term sum equals `du` of the scratch tree within
#                    1% at every sampled moment (N samples compared), and no file
#                    family under --scratch is unaccounted (unknown_paths=0).
# S2 CEILING PREDICTABLE
#                    a forced low ceiling: the governor PAUSES first (journaled),
#                    KILLS only on a sustained overrun, and the reported ceiling
#                    equals the journaled arithmetic recomputed from the reader's
#                    own numbers (free_at_start, reserve, term_sum, slack).
# S3 NO REGRESSION   the fixture's container is byte-identical to main's binary for
#                    the same input, and the band-level receipts (band count + rows
#                    per band) are identical too (N bands compared).
# S4 THE 100 GB HEAD one full build on pile-head100.txt (+ pilemap100, pos_budget
#                    20 GB): term breakdown, pause/resume behaviour, peak <= ceiling,
#                    container sha compared against a fresh main-binary build.
#
# Environment:
#   WORK=<scratch root>     default /mnt/nvme2n1/erikg/SCRATCH-CEILING
#   X=<binary under test>   default $ROOT/xsa/target/release/xsa
#   BASE_BIN=<main binary>  default $WORK/base-src/xsa/target/release/xsa, built from
#                           origin/main at the branch point (4e275f6); the sha is
#                           printed and every product comparison cites it
#   FIXBYTES  (s1,s3) default 536870912   FIXBYTES_S2 default 268435456
#   THREADS/S2THREADS/MEM/SCRATCH_GB/RUN_TIMEOUT
#   S4_CORPUS / S4_MAP / S4_POS_BUDGET / S4_THREADS / S4_MEM / S4_FINISH_AS_GB /
#   S4_SCRATCH_GB / S4_TIMEOUT
#
# Every product run is wrapped in `setsid timeout --kill-after=10 N`, and every
# gate asserts that no stage process survived its own scratch directory. No gate
# signals, stops or touches any process it did not spawn.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WORK:-/mnt/nvme2n1/erikg/SCRATCH-CEILING}
X=${X:-$HERE/xsa/target/release/xsa}
# THE REFERENCE BINARY: origin/main at the branch point (4e275f6), built ONCE from
# the commit's own tree, so the byte-identity gate never depends on whatever the
# shared checkout happens to have built:
#   git archive 4e275f6 | tar -x -C "$WORK/base-src"
#   (cd "$WORK/base-src/xsa" && cargo build --release)
BASE_BIN=${BASE_BIN:-$WORK/base-src/xsa/target/release/xsa}
FIXBYTES=${FIXBYTES:-536870912}
FIXBYTES_S2=${FIXBYTES_S2:-268435456}
THREADS=${THREADS:-16}
MEM=${MEM:-16}
S2THREADS=${S2THREADS:-16}
SCRATCH_GB=${SCRATCH_GB:-0.5}
S2_SCRATCH_GB=${S2_SCRATCH_GB:-0.5}
RUN_TIMEOUT=${RUN_TIMEOUT:-1800}
S4_CORPUS=${S4_CORPUS:-/mnt/nvme2n1/erikg/pile-head100.txt}
S4_MAP=${S4_MAP:-/mnt/nvme2n1/erikg/pilemap100}
S4_POS_BUDGET=${S4_POS_BUDGET:-20000000000}
S4_THREADS=${S4_THREADS:-96}
S4_MEM=${S4_MEM:-96}
S4_FINISH_AS_GB=${S4_FINISH_AS_GB:-400}
S4_SCRATCH_GB=${S4_SCRATCH_GB:-250}
S4_TIMEOUT=${S4_TIMEOUT:-129600}

STAGE=$WORK/stage
FX=$WORK/fixtures
LOG=$WORK/logs
mkdir -p "$STAGE" "$FX" "$LOG"

fail() { echo "SCRATCH_GOVERNOR_GATE_FAIL: $*" >&2; exit 3; }
say() { echo "$*"; }

RUN_TAGFILE=$WORK/.run_tags
: > "$RUN_TAGFILE"

# NO SURVIVORS: only OUR scratch dirs are inspected, and only by READING /proc
# (pgrep). Nothing here signals a process it did not spawn.
no_survivors() {
  local bad=""
  for tag in $(cat "$RUN_TAGFILE"); do
    local hits
    hits=$(pgrep -af "xsa_finish|scatter_sort|cross_lcp_merge|ctx-tap|xsa build" \
           | grep -F -- "$WORK/runs/$tag" || true)
    [ -z "$hits" ] || bad="$bad
$hits"
  done
  [ -z "$bad" ] || { echo "SCRATCH_GOVERNOR_SURVIVORS:$bad" >&2; fail "a stage process survived its own run"; }
  say "SCRATCH_GOVERNOR_NO_SURVIVORS checked=$(wc -w < "$RUN_TAGFILE") runs (own scratch dirs only)"
}

build_stage() {
  say "== STAGE $STAGE"
  gcc -O2 -o "$STAGE/forward_only_fixture" "$HERE/gates/forward_only_fixture.c" 2>/dev/null \
    || fail "fixture generator did not compile"
  [ -x "$X" ] || fail "no binary at $X"
  say "SCRATCH_GOVERNOR_BINARY under_test=$X sha256=$(sha256sum "$X" | cut -d' ' -f1)"
  mkdir -p "$STAGE/mainbin"
  if [ -f "$BASE_BIN" ]; then
    cp -f "$BASE_BIN" "$STAGE/mainbin/xsa-main"
    say "SCRATCH_GOVERNOR_BASE_BIN src=$BASE_BIN sha256=$(sha256sum "$STAGE/mainbin/xsa-main" | cut -d' ' -f1)"
    say "SCRATCH_GOVERNOR_BASE_GIT $(git -C "$(dirname "$(dirname "$BASE_BIN")")" log -1 --format='%H %cI' 2>/dev/null || echo unknown)"
  fi
  say "SCRATCH_GOVERNOR_BOX load=$(cut -d' ' -f1-3 /proc/loadavg) mem_available_gb=$(awk '/MemAvailable/{printf "%.0f", $2/1048576}' /proc/meminfo) work_free_gb=$(df --output=avail -BG "$WORK" | tail -1 | tr -dc 0-9)"
}

fixtures() {
  local bytes=$1 dir=$2
  if [ ! -s "$dir/out.bin" ] || [ "$(stat -c %s "$dir/out.bin")" != "$bytes" ]; then
    rm -rf "$dir"; mkdir -p "$dir"
    "$STAGE/forward_only_fixture" "$dir" "$bytes" 8192 >/dev/null || fail "fixture generation"
  fi
  say "SCRATCH_GOVERNOR_FIXTURE dir=$dir bytes=$(stat -c %s "$dir/out.bin") docs=8192"
}

# run_build BIN TAG CORPUS MAP SCRATCH_GB THREADS MEM [extra...] -> echoes the run dir
run_build() {
  local bin=$1 tag=$2 corpus=$3 map=$4 budget_gb=$5 threads=$6 mem=$7; shift 7
  local d=$WORK/runs/$tag
  echo "$tag" >> "$RUN_TAGFILE"
  rm -rf "$d"; mkdir -p "$d"
  local t0; t0=$(date +%s)
  say "SCRATCH_GOVERNOR_RUN tag=$tag bin=$bin corpus_bytes=$(stat -c %s "$corpus") budget_gb=$budget_gb threads=$threads mem_gb=$mem extra='$*'" >&2
  setsid timeout --kill-after=10 "$RUN_TIMEOUT" "$bin" build --tape \
      --input "$corpus" --scratch "$d/scratch" --docs-map "$map" \
      --threads "$threads" --memory-gb "$mem" --scratch-budget-gb "$budget_gb" \
      --trie-min-support 50 --trie-docs-min-support 50 \
      "$@" > "$d/build.log" 2>&1
  echo $? > "$d/rc"
  local t1; t1=$(date +%s); echo $((t1 - t0)) > "$d/wall_s"
  echo "$d"
}

# ---- the ceiling arithmetic, recomputed from the JOURNAL'S OWN NUMBERS --------
# Prints: ok=1|0 ceiling=<b> pause_at=<b> free_at_start=<b> reserve=<b> term_sum=<b>
#         slack=<b> fs_arm=<b> knob_arm=<b> arm=<name>
ceiling_check() {
  local log=$1
  awk '
    /^SCRATCH_GOVERNOR / {
      for (i = 1; i <= NF; i++) {
        n = split($i, kv, "=");
        if (n == 2) v[kv[1]] = kv[2];
      }
      free = v["free_at_start"] + 0; reserve = v["reserve"] + 0;
      term = v["term_sum"] + 0; slack = v["slack"] + 0;
      fs_arm = (free > reserve) ? free - reserve : 0;
      knob = term + slack;
      ceil = (fs_arm < knob) ? fs_arm : knob;
      arm = (fs_arm < knob) ? "filesystem" : "knobs";
      basis = (ceil < term) ? ceil : term;
      pause_at = int(basis / 10) * 9;
      ok = (ceil == v["ceiling"] + 0) && (fs_arm == v["fs_arm"] + 0) && \
           (knob == v["knob_arm"] + 0) && (arm == v["arm"]) && (pause_at == v["pause_at"] + 0);
      printf "ok=%d ceiling=%d pause_at=%d free_at_start=%d reserve=%d term_sum=%d slack=%d fs_arm=%d knob_arm=%d arm=%s recomputed_ceiling=%d recomputed_pause_at=%d\n",
             ok, v["ceiling"], v["pause_at"], free, reserve, term, slack, fs_arm, knob, arm, ceil, pause_at;
      exit;
    }' "$log"
}

# --------------------------------------------------------------------------- S1
s1() {
  say "== S1 TERM COVERAGE: sum(journaled terms) == du(scratch tree) within 1%, unknown_paths=0"
  fixtures "$FIXBYTES" "$FX/s1"
  local n=0 bad=0
  # THE ARMS. `auto` is the default route (on a 0.5 GiB budget the sorter picks the
  # global strategy, so term_pos legitimately reports 0 and the position term is
  # exercised at scale instead - S4 carries it). S1_LOCAL=1 adds the sorter's debug
  # arm XSA_SUBDIV_STRATEGY=local, which forces a real subdiv_pos window on disc:
  # do NOT run it on a fixture whose LCP-slice handshake is slower than the run's
  # timeout (measured: one 512 MB/0.5 GiB run finished in 303 s, another crawled in
  # the finish's repair pass and was killed at its timeout).
  local arms="auto"
  [ "${S1_LOCAL:-0}" = 1 ] && arms="auto local"
  for arm in $arms; do
    local d
    if [ "$arm" = local ]; then
      d=$(XSA_SCRATCH_AUDIT=1 XSA_SUBDIV_STRATEGY=local \
          XSA_SUBDIV_POSITION_BUDGET_BYTES=67108864 \
          run_build "$X" "s1_$arm" "$FX/s1/out.bin" "$FX/s1/map" "$SCRATCH_GB" "$THREADS" "$MEM")
    else
      d=$(XSA_SCRATCH_AUDIT=1 \
          run_build "$X" "s1_$arm" "$FX/s1/out.bin" "$FX/s1/map" "$SCRATCH_GB" "$THREADS" "$MEM")
    fi
    local rc; rc=$(cat "$d/rc")
    say "S1_ARM arm=$arm rc=$rc wall_s=$(cat "$d/wall_s")"
    [ "$rc" = 0 ] || { tail -5 "$d/build.log"; fail "S1 build ($arm) failed"; }
    local samples complete ok0 unknown worst
    samples=$(grep -c '^SCRATCH_AUDIT_TERMS' "$d/build.log" || true)
    complete=$(grep -c '^SCRATCH_AUDIT_TERMS.* du_complete=1' "$d/build.log" || true)
    ok0=$(grep -c '^SCRATCH_AUDIT_TERMS.* ok=0$' "$d/build.log" || true)
    unknown=$(grep '^SCRATCH_SAMPLE' "$d/build.log" | grep -vc 'unknown_paths=0' || true)
    worst=$(grep '^SCRATCH_AUDIT_SUMMARY' "$d/build.log" | grep -o 'worst_delta_pct=[0-9.]*' | cut -d= -f2)
    say "S1_SAMPLES arm=$arm compared=$samples du_complete=$complete over_limit=$ok0 worst_complete_delta_pct=$worst unknown_path_samples=$unknown"
    grep '^SCRATCH_AUDIT_TERMS' "$d/build.log" | tail -1 | cut -c1-260 | sed 's/^/    | /'
    grep '^SCRATCH_AUDIT_SUMMARY' "$d/build.log" | sed 's/^/    | /'
    grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | sed 's/^/    | /'
    # The families must be VISIBLE, not zero: bands, merged, pieces, container.
    local line; line=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log")
    for t in term_bands term_merged term_pieces term_container; do
      local v; v=$(echo "$line" | tr ',' '\n' | grep -o "$t=[0-9]*" | cut -d= -f2)
      [ "${v:-0}" -gt 0 ] || { say "S1_MISSING_TERM arm=$arm $t=${v:-0}"; bad=$((bad+1)); }
    done
    if [ "$arm" = local ]; then
      local p; p=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | tr ',' '\n' | grep -o 'term_pos=[0-9]*' | cut -d= -f2)
      say "S1_POS_WINDOW arm=local peak_term_pos=${p:-0}"
      [ "${p:-0}" -gt 0 ] || { say "S1_MISSING_TERM arm=local term_pos=${p:-0} (XSA_SUBDIV_STRATEGY=local wrote no window)"; bad=$((bad+1)); }
    fi
    [ "$ok0" = 0 ] && [ "$unknown" = 0 ] && [ "$complete" -ge 5 ] || bad=$((bad+1))
    n=$((n + complete))
  done
  say "S1_RESULT arms=$(echo $arms | wc -w) complete_samples_compared=$n failures=$bad"
  no_survivors
  [ "$bad" = 0 ] || fail "S1 term coverage"
}

# --------------------------------------------------------------------------- S2
s2() {
  say "== S2 CEILING PREDICTABLE: pause first, kill only on a sustained overrun"
  fixtures "$FIXBYTES_S2" "$FX/s2"
  # (a) the comfortable arm: no pause, no kill, peak <= ceiling, arithmetic exact.
  local d rc
  d=$(run_build "$X" s2_comfortable "$FX/s2/out.bin" "$FX/s2/map" "$S2_SCRATCH_GB" "$S2THREADS" "$MEM")
  rc=$(cat "$d/rc")
  local peak ceiling pauses overruns chk
  chk=$(ceiling_check "$d/build.log")
  peak=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'peak_gb=[0-9.]*' | cut -d= -f2)
  ceiling=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'ceiling_gb=[0-9.]*' | cut -d= -f2)
  pauses=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'pauses=[0-9]*' | cut -d= -f2)
  overruns=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'overruns=[0-9]*' | cut -d= -f2)
  say "S2_COMFORTABLE rc=$rc wall_s=$(cat "$d/wall_s") peak_gb=$peak ceiling_gb=$ceiling pauses=$pauses overruns=$overruns"
  say "S2_ARITHMETIC $chk"
  echo "$chk" | grep -q 'ok=1' || fail "S2: the journaled ceiling is not the journaled arithmetic"
  [ "$rc" = 0 ] || { tail -5 "$d/build.log"; fail "S2 comfortable build failed"; }
  awk -v p="$peak" -v c="$ceiling" 'BEGIN{exit !(p+0 <= c+0)}' \
    || fail "S2: the peak ($peak GB) exceeded the ceiling ($ceiling GB) on the comfortable arm"

  # (b) the forced-low-ceiling arm: the reserve is pushed until the FILESYSTEM arm
  #     binds BELOW the measured peak. The free space of a shared box moves, so
  #     this is retried (up to 3 attempts) and every attempt prints its numbers.
  local attempt=1 killed=0
  while [ "$attempt" -le 3 ] && [ "$killed" = 0 ]; do
    local avail avail_gb target_gb reserve_gb
    avail=$(df -B1 --output=avail "$WORK" | tail -1 | tr -dc 0-9)
    avail_gb=$(awk -v a="$avail" 'BEGIN{printf "%.6f", a/1073741824.0}')
    # aim the FILESYSTEM arm at 35% of the measured peak: below it by construction,
    # so a pause and then the bounded kill are the only possible outcomes.
    target_gb=$(awk -v p="$peak" 'BEGIN{printf "%.6f", p*0.35}')
    reserve_gb=$(awk -v a="$avail_gb" -v t="$target_gb" 'BEGIN{printf "%.6f", a-t}')
    say "S2_ATTEMPT n=$attempt avail_gb=$avail_gb target_ceiling_gb=$target_gb reserve_gb=$reserve_gb"
    d=$(run_build "$X" "s2_forced_$attempt" "$FX/s2/out.bin" "$FX/s2/map" "$S2_SCRATCH_GB" "$S2THREADS" "$MEM" \
        --scratch-reserve-gb "$reserve_gb")
    rc=$(cat "$d/rc")
    chk=$(ceiling_check "$d/build.log")
    say "S2_FORCED attempt=$attempt rc=$rc wall_s=$(cat "$d/wall_s")"
    say "S2_ARITHMETIC attempt=$attempt $chk"
    echo "$chk" | grep -q 'ok=1' || fail "S2: the journaled ceiling is not the journaled arithmetic (forced arm)"
    if [ "$rc" != 0 ] && grep -q 'SCRATCH_GOVERNOR_CEILING_EXCEEDED' "$d/build.log"; then
      killed=1
      say "S2_PAUSE_THEN_KILL attempt=$attempt"
      grep -E '^SCRATCH_BACKPRESSURE state=pause|^SCRATCH_OVERRUN|^SCRATCH_GOVERNOR signal=KILL' "$d/build.log" \
        | cut -c1-220 | sed 's/^/    | /'
      local np nk
      np=$(grep -c '^SCRATCH_BACKPRESSURE state=pause' "$d/build.log" || true)
      nk=$(grep -c '^SCRATCH_OVERRUN' "$d/build.log" || true)
      say "S2_ORDER pause_events=$np overrun_samples=$nk (kill_after_samples=3 -> 4 overrun samples)"
      [ "$np" -ge 1 ] || fail "S2: no pause was journaled before the kill"
      [ "$nk" = 4 ] || say "S2_NOTE overrun_samples=$nk (expected 4: the sample that pauses plus 3 in the drain window)"
      # The FIRST response must be the pause, not the kill: the pause line must
      # precede the first overrun line and the kill line.
      local lp lo lk
      lp=$(grep -n '^SCRATCH_BACKPRESSURE state=pause' "$d/build.log" | head -1 | cut -d: -f1)
      lo=$(grep -n '^SCRATCH_OVERRUN' "$d/build.log" | head -1 | cut -d: -f1)
      lk=$(grep -n 'SCRATCH_GOVERNOR signal=KILL' "$d/build.log" | head -1 | cut -d: -f1)
      say "S2_LINE_ORDER pause=$lp overrun=$lo kill=$lk"
      [ "$lp" -lt "$lo" ] && [ "$lo" -lt "$lk" ] || fail "S2: the pause did not precede the overrun/kill"
      grep -E '^SCRATCH_GOVERNOR_CEILING' "$d/build.log" | cut -c1-600 | sed 's/^/    | /'
      rm -rf "$d/scratch"   # the killed build retains its tree; drop it now
    else
      say "S2_RETRY attempt=$attempt did not overrun (free space moved between df and plan time)"
      rm -rf "$d/scratch"
    fi
    attempt=$((attempt + 1))
  done
  [ "$killed" = 1 ] || fail "S2: no forced-low-ceiling attempt reached the kill"
  no_survivors
  say "S2_RESULT ok=1"
}

# --------------------------------------------------------------------------- S3
s3() {
  say "== S3 NO REGRESSION: container byte-identity against main's binary"
  fixtures "$FIXBYTES" "$FX/s3"
  local ours main rc_ours rc_main
  ours=$(run_build "$X" s3_ours "$FX/s3/out.bin" "$FX/s3/map" "$SCRATCH_GB" "$THREADS" "$MEM")
  rc_ours=$(cat "$ours/rc")
  [ "$rc_ours" = 0 ] || { tail -5 "$ours/build.log"; fail "S3: our build failed"; }
  [ -x "$STAGE/mainbin/xsa-main" ] || fail "S3: no main binary at $BASE_BIN"
  main=$(run_build "$STAGE/mainbin/xsa-main" s3_main "$FX/s3/out.bin" "$FX/s3/map" "$SCRATCH_GB" "$THREADS" "$MEM")
  rc_main=$(cat "$main/rc")
  [ "$rc_main" = 0 ] || { tail -5 "$main/build.log"; fail "S3: the main-binary build failed"; }
  local co cm
  co=$(sha256sum "$ours/scratch/out.sxi2" | cut -d' ' -f1)
  cm=$(sha256sum "$main/scratch/out.sxi2" | cut -d' ' -f1)
  say "S3_CONTAINER ours=$co bytes=$(stat -c %s "$ours/scratch/out.sxi2")"
  say "S3_CONTAINER main=$cm bytes=$(stat -c %s "$main/scratch/out.sxi2")"
  [ "$co" = "$cm" ] || fail "S3: the containers differ"
  # THE BAND-LEVEL RECEIPTS: the sorter journals one SS_BAND line per band with its
  # row count; a band whose content changed would change (band, rows). The lines are
  # emitted by concurrent writer slots, so the ORDER in the log is not a contract -
  # the SET is, and it is compared sorted.
  local bo bm nb
  bo=$(grep -oE '^SS_BAND band=[0-9]+ rows=[0-9]+' "$ours/scratch/order-tape.log" | sort | sha256sum | cut -d' ' -f1)
  bm=$(grep -oE '^SS_BAND band=[0-9]+ rows=[0-9]+' "$main/scratch/order-tape.log" | sort | sha256sum | cut -d' ' -f1)
  nb=$(grep -cE '^SS_BAND ' "$ours/scratch/order-tape.log" || true)
  say "S3_BANDS compared=$nb (band,rows) pairs ours_receipts_sha=$bo main_receipts_sha=$bm"
  [ "$bo" = "$bm" ] || fail "S3: the per-band receipts differ"
  say "S3_WALLS ours_s=$(cat "$ours/wall_s") main_s=$(cat "$main/wall_s")"
  grep -E '^SCRATCH_BACKPRESSURE_SUMMARY' "$ours/build.log" | sed 's/^/    | ours /'
  grep -E '^SCRATCH_BACKPRESSURE_SUMMARY' "$main/build.log" | sed 's/^/    | main /'
  no_survivors
  say "S3_RESULT ok=1 bands=$nb container_sha=$co"
}

# --------------------------------------------------------------------------- S6
# THE BOX-FREEZE GATE. The inherited pause path shelled out to
# `kill -<SIG> -<pgid>`; procps-ng 4.0.4 keeps only the FIRST DIGIT of a negative
# pid spec, so `kill -STOP -1257815` issued kill(-1, SIGSTOP) = STOP EVERY PROCESS
# OF THE USER, and the ceiling KILL would have been kill(-1, SIGKILL). Measured:
#   strace -e kill /usr/bin/kill -0 -1257815  ->  kill(-1, 0)
#   strace -e kill /usr/bin/kill -0 -2449516  ->  kill(-2, 0)
# This gate asserts, on a REAL product run that pauses and kills, that every
# negative-pid kill(2) is sign-correct with the WHOLE group id, that -1 is never
# used, and that no shell or kill(1) is ever executed.
s6() {
  say "== S6 SIGSTOP/SIGCONT/SIGKILL TARGETS: the sign-correct group id, never kill(-1), no kill(1)"
  fixtures "$FIXBYTES_S2" "$FX/s2"
  local avail avail_gb peak target_gb reserve_gb d rc
  peak=$(grep -m1 '^SCRATCH_BACKPRESSURE_SUMMARY' "$WORK/runs/s2_comfortable/build.log" 2>/dev/null \
         | grep -o 'peak_gb=[0-9.]*' | cut -d= -f2)
  [ -n "$peak" ] || fail "S6: run the s2 gate first (it measures the fixture's peak)"
  avail=$(df -B1 --output=avail "$WORK" | tail -1 | tr -dc 0-9)
  avail_gb=$(awk -v a="$avail" 'BEGIN{printf "%.6f", a/1073741824.0}')
  target_gb=$(awk -v p="$peak" 'BEGIN{printf "%.6f", p*0.35}')
  reserve_gb=$(awk -v a="$avail_gb" -v t="$target_gb" 'BEGIN{printf "%.6f", a-t}')
  # The free space of a shared box moves between df and plan time, so the forced
  # ceiling is retried with a tighter target (35%, then 20%, then 10% of the peak).
  local attempt=1 fired=0
  while [ "$attempt" -le 3 ] && [ "$fired" = 0 ]; do
    local tagn=s6_signal
    echo "$tagn" >> "$RUN_TAGFILE"
    d=$WORK/runs/$tagn
    rm -rf "$d"; mkdir -p "$d"
    avail=$(df -B1 --output=avail "$WORK" | tail -1 | tr -dc 0-9)
    avail_gb=$(awk -v a="$avail" 'BEGIN{printf "%.6f", a/1073741824.0}')
    target_gb=$(awk -v p="$peak" -v a="$attempt" 'BEGIN{printf "%.6f", p*(a==1?0.35:(a==2?0.20:0.10))}')
    reserve_gb=$(awk -v a="$avail_gb" -v t="$target_gb" 'BEGIN{printf "%.6f", a-t}')
    say "S6_RUN attempt=$attempt reserve_gb=$reserve_gb target_ceiling_gb=$target_gb (file system arm aimed below the measured peak $peak GB)"
    setsid timeout --kill-after=10 "$RUN_TIMEOUT" strace -f -qq -e trace=kill,execve \
        -o "$d/strace.log" "$X" build --tape --input "$FX/s2/out.bin" --scratch "$d/scratch" \
        --docs-map "$FX/s2/map" --threads "$S2THREADS" --memory-gb "$MEM" \
        --scratch-budget-gb "$S2_SCRATCH_GB" --trie-min-support 50 --trie-docs-min-support 50 \
        --scratch-reserve-gb "$reserve_gb" > "$d/build.log" 2>&1
    rc=$?
    if grep -q '^SCRATCH_BACKPRESSURE signal=' "$d/build.log"; then fired=1; else
      say "S6_RETRY attempt=$attempt: no signal was journaled (free space moved between df and plan time)"
      rm -rf "$d/scratch"
    fi
    attempt=$((attempt + 1))
  done
  [ "$fired" = 1 ] || fail "S6: no attempt reached the pause path"
  say "S6_RC rc=$rc (a forced-ceiling run: the pause and the bounded kill are the expected path)"
  say "S6_SYSCALLS $(printf 'kill_calls=%s negative_pid_calls=%s' \
      "$(grep -c 'kill(' "$d/strace.log" || true)" \
      "$(grep -c 'kill(-' "$d/strace.log" || true)")"
  grep -o 'kill(-[0-9]*, SIG[A-Z]*' "$d/strace.log" | sed 's/$/)/' | sort | uniq -c | sed 's/^/    | /'
  # 1. no kill(-1) / kill(0) / kill(-N) with a one-digit group
  local bad
  bad=$(grep -o 'kill(-[0-9]*, SIG' "$d/strace.log" | sed 's/kill(-\([0-9]*\).*/\1/' \
        | awk '$1 <= 1000' | sort -u | tr '\n' ' ')
  say "S6_TRUNCATED_OR_SPECIAL_TARGETS '${bad}'"
  [ -z "$bad" ] || fail "S6: a kill(2) was aimed at a truncated/special pid: $bad"
  # 2. EVERY JOURNALED GOVERNOR SIGNAL IS ONE kill(2) WITH THE WHOLE GROUP ID:
  #    the journal says `signal=STOP phase=X pgid=1257815`, so the syscall must be
  #    kill(-1257815, SIGSTOP) - not kill(-1, ..), not kill(-2, ..).
  local journal_pgids strace_pgids pairs checked=0
  journal_pgids=$(grep -oE 'SCRATCH_(BACKPRESSURE|GOVERNOR) signal=[A-Z]* phase=[a-z0-9-]* pgid=[0-9]*' "$d/build.log" \
                  | sed -E 's/SCRATCH_(BACKPRESSURE|GOVERNOR) signal=([A-Z]*) phase=[a-z0-9-]* pgid=([0-9]*)/\2,\3/' \
                  | sort -u)
  strace_pgids=$(grep -o 'kill(-[0-9]*, SIG' "$d/strace.log" | sed 's/kill(-\([0-9]*\).*/\1/' | sort -u | tr '\n' ' ')
  say "S6_JOURNAL_SIGNALS '$(echo $journal_pgids)'"
  say "S6_SYSCALL_PGIDS '$strace_pgids'"
  [ -n "$journal_pgids" ] || fail "S6: the run journaled no pause/kill signal at all (nothing to check)"
  for pairs in $journal_pgids; do
    local sig=${pairs%%,*} pgid=${pairs##*,}
    grep -q "kill(-$pgid, SIG$sig" "$d/strace.log" \
      || fail "S6: the journal names signal=$sig pgid=$pgid but no kill(-$pgid, SIG$sig) syscall was made"
    checked=$((checked + 1))
  done
  say "S6_JOURNALED_SIGNALS_VERIFIED $checked (each one syscall, sign-correct, whole group id)"
  # The remaining negative-pid kills are the phase-cleanup path (RunningPhase::drop),
  # which has always used kill(2) directly; list them so the record is complete.
  say "S6_ALL_NEGATIVE_PID_KILLS '$(grep -o 'kill(-[0-9]*, SIG[A-Z]*' "$d/strace.log" | sed 's/$/)/' | sort | uniq -c | tr '\n' ' ')'"
  # 3. kill(1) IS NEVER EXECUTED, AND NO SHELL IS USED AS ITS VEHICLE. (Shells are
  #    legitimate here for other reasons: PHASE_WRAPPER is `bash -c ulimit -v ...`
  #    and the driver runs `mkdir -p`; what must never happen is a `kill` binary or
  #    a `sh -c 'kill ...'`.)
  local kill1 shellkill
  kill1=$(grep -cE 'execve\("(/usr)?/bin/kill"' "$d/strace.log" || true)
  shellkill=$(grep -cE 'execve\("(/usr)?/bin/(sh|bash|dash)".*(kill[ \"]|/bin/kill)' "$d/strace.log" || true)
  say "S6_KILL1_EXECVE $kill1 S6_SHELL_KILL_EXECVE $shellkill"
  [ "$kill1" = 0 ] && [ "$shellkill" = 0 ] \
    || { grep -E 'execve\("(/usr)?/bin/(kill|sh|bash|dash)".*(kill[ \"]|/bin/kill)' "$d/strace.log" | sed 's/^/    | /'; fail "S6: kill(1) or a shell-wrapped kill was executed"; }
  rm -rf "$d/scratch"
  no_survivors
  say "S6_RESULT ok=1 sign_correct_group_ids=1 kill_minus_one=0 kill1_execve=0"
}

# --------------------------------------------------------------------------- S4
s4() {
  say "== S4 THE 100 GB HEAD: pile-head100 + pilemap100, pos_budget 20 GB"
  [ -f "$S4_CORPUS" ] || fail "S4: no corpus at $S4_CORPUS"
  [ -d "$S4_MAP" ] || fail "S4: no map at $S4_MAP"
  [ -x "$STAGE/mainbin/xsa-main" ] || fail "S4: no main binary at $BASE_BIN"
  local n=0
  for arm in ours main; do
    local bin=$X
    [ "$arm" = main ] && bin=$STAGE/mainbin/xsa-main
    local d=$WORK/runs/s4_$arm
    echo "s4_$arm" >> "$RUN_TAGFILE"
    rm -rf "$d"; mkdir -p "$d"
    say "S4_BOX arm=$arm load=$(cut -d' ' -f1-3 /proc/loadavg) other_heavy_procs=$(pgrep -c -f 'scatter_sort|xsa build|xsa_finish' || true) free_gb=$(df --output=avail -BG "$WORK" | tail -1 | tr -dc 0-9)"
    say "S4_RUN arm=$arm bin=$bin sha256=$(sha256sum "$bin" | cut -d' ' -f1) corpus_bytes=$(stat -c %s "$S4_CORPUS") map=$S4_MAP pos_budget=$S4_POS_BUDGET threads=$S4_THREADS mem_gb=$S4_MEM finish_as_gb=$S4_FINISH_AS_GB scratch_budget_gb=$S4_SCRATCH_GB"
    local t0; t0=$(date +%s)
    env XSA_SUBDIV_POSITION_BUDGET_BYTES="$S4_POS_BUDGET" \
      setsid timeout --kill-after=10 "$S4_TIMEOUT" "$bin" build --tape \
        --input "$S4_CORPUS" --scratch "$d/scratch" --docs-map "$S4_MAP" \
        --threads "$S4_THREADS" --memory-gb "$S4_MEM" --finish-as-gb "$S4_FINISH_AS_GB" \
        --scratch-budget-gb "$S4_SCRATCH_GB" \
        --trie-depth 8 --trie-min-support 1000 > "$d/build.log" 2>&1
    echo $? > "$d/rc"
    local t1; t1=$(date +%s); echo $((t1 - t0)) > "$d/wall_s"
    local rc; rc=$(cat "$d/rc")
    say "S4_DONE arm=$arm rc=$rc wall_s=$(cat "$d/wall_s") box_load_at_end=$(cut -d' ' -f1-3 /proc/loadavg)"
    [ "$rc" = 0 ] || { tail -20 "$d/build.log"; fail "S4: the $arm build failed"; }
    grep -E "^(XSA_PLAN_TAPE|XSA_PLAN_DISC|SCRATCH_BUDGET |SCRATCH_GOVERNOR|SCRATCH_TERMS)" "$d/build.log" | cut -c1-700 | sed 's/^/    | /'
    grep -E '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | sed 's/^/    | /'
    say "S4_ARITHMETIC arm=$arm $(ceiling_check "$d/build.log")"
    local peak ceiling pauses resumes
    peak=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'peak_gb=[0-9.]*' | cut -d= -f2)
    ceiling=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'ceiling_gb=[0-9.]*' | cut -d= -f2)
    pauses=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'pauses=[0-9]*' | cut -d= -f2)
    resumes=$(grep '^SCRATCH_BACKPRESSURE_SUMMARY' "$d/build.log" | grep -o 'resumes=[0-9]*' | cut -d= -f2)
    awk -v p="$peak" -v c="$ceiling" 'BEGIN{exit !(p+0 <= c+0)}' \
      || fail "S4: arm=$arm peak_gb=$peak exceeded ceiling_gb=$ceiling"
    say "S4_PEAK arm=$arm peak_gb=$peak ceiling_gb=$ceiling pauses=$pauses resumes=$resumes under_ceiling=1"
    # the term breakdown at the peak sample: the sample with the largest live
    local best
    best=$(grep '^SCRATCH_SAMPLE' "$d/build.log" | awk '{for(i=1;i<=NF;i++) if($i ~ /^live=/){split($i,a,"="); print a[2], $0}}' | sort -n | tail -1 | cut -d' ' -f2-)
    say "S4_PEAK_SAMPLE arm=$arm $(echo "$best" | cut -c1-500)"
    say "S4_CONTAINER arm=$arm bytes=$(stat -c %s "$d/scratch/pile-head100.sxi2") sha256=$(sha256sum "$d/scratch/pile-head100.sxi2" | cut -d' ' -f1)"
    grep -E '^XSA_BUILD_DONE' "$d/build.log" | sed 's/^/    | /'
    n=$((n + 1))
  done
  local co cm
  co=$(sha256sum "$WORK/runs/s4_ours/scratch/pile-head100.sxi2" | cut -d' ' -f1)
  cm=$(sha256sum "$WORK/runs/s4_main/scratch/pile-head100.sxi2" | cut -d' ' -f1)
  say "S4_IDENTITY ours=$co main=$cm identical=$([ "$co" = "$cm" ] && echo 1 || echo 0)"
  [ "$co" = "$cm" ] || fail "S4: the 100 GB containers differ"
  no_survivors
  say "S4_RESULT ok=1 arms=$n container_sha=$co"
}

case "${1:-all}" in
  s1) build_stage; s1 ;;
  s2) build_stage; s2 ;;
  s3) build_stage; s3 ;;
  s4) build_stage; s4 ;;
  s6) build_stage; s6 ;;
  all) build_stage; s1; s2; s3; s6 ;;
  *) fail "usage: $0 s1|s2|s3|s4|s6|all" ;;
esac
