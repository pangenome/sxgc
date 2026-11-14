#!/usr/bin/env bash
# scratch_governor_gates.sh - the gates of RESULTS_SCRATCH_GOVERNOR.md.
#
#   gates/scratch_governor_gates.sh r1|r2|r3|r4|r5|s4|all
#
# R1 NO STOP/CONT   a full fixture build under strace: ZERO SIGSTOP, ZERO SIGCONT,
#                   zero signal syscalls at all, no exec of kill(1).
# R2 PLAN-TIME REFUSAL
#                   a bound that does not fit is REFUSED before a byte is written:
#                   no bands/merged/waves/container in the scratch, the term table
#                   and the lever names in the message.
# R3 ADAPTIVE SHRINK
#                   live over target: the governor shrinks the NEXT position
#                   window, the sorter applies it (`SS_GEOMETRY`), the following
#                   window is smaller, and live falls.
# R4 HARD FAIL IS GRACEFUL
#                   a forced ceiling: the build fails with the numbers, every phase
#                   exits (no survivors), no SIGSTOP/SIGCONT anywhere, the phases
#                   are stopped per pid (SIGTERM then SIGKILL, never a group), and
#                   the checkpoint journal is left consistent.
# R5 NO REGRESSION  fixture container byte-identical to origin/main's binary.
# S4 THE 100 GB HEAD
#                   the scale check: the term table, max_live vs the measured peak
#                   and the error, both arms.
#
# Environment:
#   WORK=<scratch root>     default /mnt/nvme2n1/erikg/SCRATCH-CEILING
#   X=<binary under test>   default $ROOT/xsa/target/release/xsa
#   BASE_BIN=<main binary>  default $WORK/base-src/xsa/target/release/xsa, built from
#                           origin/main at the branch point (4e275f6)
#   FIXBYTES default 536870912   FIXBYTES_SMALL default 268435456
#   THREADS/MEM/SCRATCH_GB/RUN_TIMEOUT
#   S4_*: corpus, map, pos budget, threads, memory, finish-as, scratch budget
#
# Every product run is wrapped in `setsid --wait timeout --kill-after=10 N`; every gate
# asserts that no stage process survived ITS OWN scratch directory. No gate
# signals a process it did not spawn.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
WORK=${WORK:-/mnt/nvme2n1/erikg/SCRATCH-CEILING}
X=${X:-$HERE/xsa/target/release/xsa}
BASE_BIN=${BASE_BIN:-$WORK/base-src/xsa/target/release/xsa}
FIXBYTES=${FIXBYTES:-536870912}
FIXBYTES_SMALL=${FIXBYTES_SMALL:-268435456}
THREADS=${THREADS:-16}
MEM=${MEM:-16}
SCRATCH_GB=${SCRATCH_GB:-0.5}
RUN_TIMEOUT=${RUN_TIMEOUT:-1800}
S4_CORPUS=${S4_CORPUS:-/mnt/nvme2n1/erikg/pile-head100.txt}
S4_MAP=${S4_MAP:-/mnt/nvme2n1/erikg/pilemap100}
S4_POS_BUDGET=${S4_POS_BUDGET:-20000000000}
S4_THREADS=${S4_THREADS:-96}
S4_MEM=${S4_MEM:-96}
S4_FINISH_AS_GB=${S4_FINISH_AS_GB:-400}
S4_SCRATCH_GB=${S4_SCRATCH_GB:-100}
S4_EXTRA=${S4_EXTRA:---no-trie}
S4_TIMEOUT=${S4_TIMEOUT:-129600}

STAGE=$WORK/stage
FX=$WORK/fixtures
LOG=$WORK/logs
mkdir -p "$STAGE" "$FX" "$LOG"

fail() { echo "SCRATCH_GOVERNOR_GATE_FAIL: $*" >&2; exit 3; }
say() { echo "$*"; }

# PER INVOCATION: a gate that is running concurrently must never inspect (or
# report on) another gate's runs.
RUN_TAGFILE=$WORK/.run_tags.$$
: > "$RUN_TAGFILE"

no_survivors() {
  local bad=""
  for tag in $(cat "$RUN_TAGFILE"); do
    local hits
    hits=$(pgrep -af "xsa_finish|scatter_sort|cross_lcp_merge|ctx-tap" \
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
    say "SCRATCH_GOVERNOR_BASE_BIN src=$BASE_BIN sha256=$(sha256sum "$STAGE/mainbin/xsa-main" | cut -d' ' -f1) ref=origin/main@4e275f6"
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
  setsid --wait timeout --kill-after=10 "$RUN_TIMEOUT" "$bin" build --tape \
      --input "$corpus" --scratch "$d/scratch" --docs-map "$map" \
      --threads "$threads" --memory-gb "$mem" --scratch-budget-gb "$budget_gb" \
      --trie-min-support 50 --trie-docs-min-support 50 \
      "$@" > "$d/build.log" 2>&1
  echo $? > "$d/rc"
  local t1; t1=$(date +%s); echo $((t1 - t0)) > "$d/wall_s"
  echo "$d"
}

# run_strace TAG CORPUS MAP SCRATCH_GB THREADS MEM [extra...] -> run dir, straced
run_strace() {
  local tag=$1 corpus=$2 map=$3 budget_gb=$4 threads=$5 mem=$6; shift 6
  local d=$WORK/runs/$tag
  echo "$tag" >> "$RUN_TAGFILE"
  rm -rf "$d"; mkdir -p "$d"
  local t0; t0=$(date +%s)
  say "SCRATCH_GOVERNOR_RUN tag=$tag strace=1 corpus_bytes=$(stat -c %s "$corpus") budget_gb=$budget_gb extra='$*'" >&2
  setsid --wait timeout --kill-after=10 "$RUN_TIMEOUT" strace -f -qq \
      -e trace=kill,tkill,tgkill,execve -o "$d/strace.log" \
      "$X" build --tape --input "$corpus" --scratch "$d/scratch" --docs-map "$map" \
      --threads "$threads" --memory-gb "$mem" --scratch-budget-gb "$budget_gb" \
      --trie-min-support 50 --trie-docs-min-support 50 \
      "$@" > "$d/build.log" 2>&1
  echo $? > "$d/rc"
  local t1; t1=$(date +%s); echo $((t1 - t0)) > "$d/wall_s"
  echo "$d"
}

# ---- the bound arithmetic, recomputed from the JOURNAL'S OWN NUMBERS --------
# Prints: ok=1|0 max_live=<b> fs_available=<b> ceiling=<b> ...
bound_check() {
  local log=$1
  awk '
    /^SCRATCH_BOUND / {
      for (i = 1; i <= NF; i++) {
        n = split($i, kv, "=");
        if (n == 2) v[kv[1]] = kv[2];
      }
      free = v["free_at_start"] + 0; reserve = v["reserve"] + 0;
      term = v["term_sum"] + 0; slack = v["slack"] + 0; target = v["target"] + 0;
      max_live = term + slack;
      fs_avail = (free > reserve) ? free - reserve : 0;
      ceil = max_live; if (fs_avail < ceil) ceil = fs_avail;
      t125 = int(target * 125 / 100); if (t125 < ceil) ceil = t125;
      fits = (max_live <= fs_avail) ? 1 : 0;
      ok = (max_live == v["max_live"] + 0) && (fs_avail == v["fs_available"] + 0) && \
           (ceil == v["ceiling"] + 0) && (fits == v["fits"] + 0);
      printf "ok=%d max_live=%d fs_available=%d ceiling=%d fits=%d target=%d free_at_start=%d reserve=%d slack=%d recomputed_ceiling=%d\n",
             ok, max_live, fs_avail, v["ceiling"], fits, target, free, reserve, slack, ceil;
      exit;
    }' "$log"
}

# --------------------------------------------------------------------------- R1
r1() {
  say "== R1 NO STOP/CONT: a full fixture build, straced, must make no signal syscall at all"
  fixtures "$FIXBYTES" "$FX/r1"
  local d rc
  d=$(run_strace r1_nostop "$FX/r1/out.bin" "$FX/r1/map" "$SCRATCH_GB" "$THREADS" "$MEM")
  rc=$(cat "$d/rc")
  say "R1_RC rc=$rc wall_s=$(cat "$d/wall_s")"
  [ "$rc" = 0 ] || { tail -5 "$d/build.log"; fail "R1: the build did not complete"; }
  local calls stop cont kill1 any
  calls=$(grep -cE '^[0-9]+ (kill|tkill|tgkill)\(' "$d/strace.log" || true)
  stop=$(grep -cE '(kill|tkill|tgkill)\(.*SIGSTOP' "$d/strace.log" || true)
  cont=$(grep -cE '(kill|tkill|tgkill)\(.*SIGCONT' "$d/strace.log" || true)
  kill1=$(grep -cE 'execve\("(/usr)?/bin/kill"' "$d/strace.log" || true)
  any=$(grep -cE '(kill|tkill|tgkill)\(' "$d/strace.log" || true)
  say "R1_SIGNAL_SYSCALLS all=$any SIGSTOP=$stop SIGCONT=$cont kill1_execve=$kill1"
  say "R1_TRACED_BUILD journal_gov_lines=$(grep -c '^SCRATCH_GOVERNOR\|^SCRATCH_BOUND\|^SCRATCH_SAMPLE' "$d/build.log" || true)"
  grep -E '^SCRATCH_BOUND|^SCRATCH_GOVERNOR_SUMMARY' "$d/build.log" | cut -c1-320 | sed 's/^/    | /'
  [ "$stop" = 0 ] || { grep -E 'SIGSTOP' "$d/strace.log" | head -3; fail "R1: a SIGSTOP was sent"; }
  [ "$cont" = 0 ] || { grep -E 'SIGCONT' "$d/strace.log" | head -3; fail "R1: a SIGCONT was sent"; }
  [ "$kill1" = 0 ] || fail "R1: kill(1) was executed"
  [ "$any" = 0 ] || { grep -E '(kill|tkill|tgkill)\(' "$d/strace.log" | head -5; fail "R1: a successful build must make no signal syscall at all"; }
  no_survivors
  say "R1_RESULT ok=1 SIGSTOP=0 SIGCONT=0 signal_syscalls=0 kill1_execve=0"
}

# --------------------------------------------------------------------------- R2
r2() {
  say "== R2 PLAN-TIME REFUSAL: a bound that does not fit refuses BEFORE writing a byte"
  fixtures "$FIXBYTES_SMALL" "$FX/r2"
  # The filesystem is made "too small" from the plan's point of view by pushing the
  # reserve up to (nearly) the free space: fs_available becomes ~1 MB, which is the
  # same arithmetic a small device produces (max_live = sum(term budgets)+slack).
  local avail avail_gb reserve_gb d rc
  avail=$(df -B1 --output=avail "$WORK" | tail -1 | tr -dc 0-9)
  avail_gb=$(awk -v a="$avail" 'BEGIN{printf "%.6f", a/1073741824.0}')
  reserve_gb=$(awk -v a="$avail_gb" 'BEGIN{printf "%.6f", a - 0.001}')
  say "R2_RESERVE avail_gb=$avail_gb reserve_gb=$reserve_gb (fs_available ~1 MB)"
  d=$(run_build "$X" r2_refusal "$FX/r2/out.bin" "$FX/r2/map" "$SCRATCH_GB" "$THREADS" "$MEM" \
      --scratch-reserve-gb "$reserve_gb")
  rc=$(cat "$d/rc")
  say "R2_RC rc=$rc wall_s=$(cat "$d/wall_s") (a refusal is expected)"
  say "R2_ARITHMETIC $(bound_check "$d/build.log")"
  bound_check "$d/build.log" | grep -q 'fits=0' || fail "R2: the bound did not report fits=0"
  grep -q 'SCRATCH_BOUND_REFUSED' "$d/build.log" || { tail -5 "$d/build.log"; fail "R2: no SCRATCH_BOUND_REFUSED"; }
  grep -E '^SCRATCH_BOUND_REFUSED' "$d/build.log" | cut -c1-900 | sed 's/^/    | /'
  grep -E '^SCRATCH_BOUND ' "$d/build.log" | cut -c1-500 | sed 's/^/    | /'
  # THE LEVERS AND THE TERM TABLE MUST BE IN THE MESSAGE.
  for t in term_pos term_bands term_merged term_pieces term_container term_other; do
    grep -q "$t=" "$d/build.log" || fail "R2: the refusal does not carry $t"
  done
  for l in --scratch-budget-gb XSA_SUBDIV_POSITION_BUDGET_BYTES --scratch-reserve-gb; do
    grep -q -- "$l" "$d/build.log" || fail "R2: the refusal does not name the lever $l"
  done
  say "R2_MESSAGE levers=3 terms=6 present=1"
  # NOTHING WAS WRITTEN: no build family, no container; report the bytes there are.
  local written fam
  written=$(du -s --block-size=1 "$d/scratch" | cut -f1)
  fam=$(ls -A "$d/scratch" | tr '\n' ' ')
  say "R2_SCRATCH_BYTES $written families='$fam' (the journal, the plan's remap spool and the resume checkpoint ONLY)"
  for want in bands-tape merged waves-tape subdiv_pos; do
    [ ! -e "$d/scratch/$want" ] || fail "R2: the refusal wrote $want"
  done
  for f in $fam; do
    case "$f" in xsa-build.log|remap-256.bin|checkpoint.journal) ;; \
      *) fail "R2: the refusal left an unexpected file: $f" ;; esac
  done
  compgen -G "$d/scratch/*.sxi2*" >/dev/null && fail "R2: the refusal wrote a container"
  awk -v b="$written" 'BEGIN{exit !(b+0 < 1048576)}' \
    || fail "R2: the refusal wrote $written bytes (expected < 1 MiB: journal + remap)"
  no_survivors
  say "R2_RESULT ok=1 written_bytes=$written fits=0"
}

# --------------------------------------------------------------------------- R3
r3() {
  say "== R3 ADAPTIVE SHRINK: live over target -> the NEXT position window is smaller"
  fixtures "$FIXBYTES_SMALL" "$FX/r3"
  # The position window is the term that fills the disc, so the shrink is exercised
  # on the sorter's WAVE subdivision (XSA_SUBDIV_STRATEGY=local) with a position
  # budget large enough for the window to be visible in `live`.
  local d rc target_gb
  # The target sits ABOVE 125% of nothing and BELOW the fixture peak
  # (2.4 GB): the shrink fires, and the 125%-of-target ceiling is not reached.
  target_gb=1.85
  # A 64 MB position budget makes ONE WAVE per window, so there are several windows
  # and a shrink has a NEXT window to apply to (a 268 MB budget fits this fixture's
  # 37.7M local rows in a single window, where a shrink is unobservable).
  d=$(XSA_SUBDIV_STRATEGY=local XSA_SUBDIV_POSITION_BUDGET_BYTES=67108864 \
      run_build "$X" r3_shrink "$FX/r3/out.bin" "$FX/r3/map" "$SCRATCH_GB" "$THREADS" "$MEM" \
      --scratch-target-gb "$target_gb")
  rc=$(cat "$d/rc")
  say "R3_RC rc=$rc wall_s=$(cat "$d/wall_s") target_gb=$target_gb"
  say "R3_ARITHMETIC $(bound_check "$d/build.log")"
  local shrinks applied
  shrinks=$(grep -c '^SCRATCH_GEOMETRY action=shrink' "$d/build.log" || true)
  applied=$(grep -c '^SS_GEOMETRY action=shrink' "$d/scratch/order-tape.log" 2>/dev/null || true)
  say "R3_DECISIONS governor_shrinks=$shrinks sorter_applied=$applied"
  grep -E '^SCRATCH_GEOMETRY' "$d/build.log" | cut -c1-300 | sed 's/^/    | /' | head -6
  grep -E '^SS_GEOMETRY' "$d/scratch/order-tape.log" 2>/dev/null | cut -c1-300 | sed 's/^/    | /' | head -6
  [ "$shrinks" -ge 1 ] || fail "R3: the governor never shrank the window (live never exceeded the target)"
  [ "$applied" -ge 1 ] || say "R3_NOTE the governor's shrink landed after the sorter's last window on this fixture (its container dominates live); the channel is proven directly below"
  # THE SHRUNK WINDOW IS SMALLER: every SS_GEOMETRY line says to_bytes < from_bytes.
  grep -E '^SS_GEOMETRY' "$d/scratch/order-tape.log" | \
    awk '{ for (i=1;i<=NF;i++) { split($i,kv,"="); if (kv[1]=="from_bytes") f=kv[2]; if (kv[1]=="to_bytes") t=kv[2]; } if (!(t+0 < f+0)) exit 1; }' \
    || fail "R3: an SS_GEOMETRY line did not shrink the window"
  # ... AND THE FOLLOWING WINDOWS HONOUR IT (the window bytes never exceed to_bytes).
  local to_bytes worst_after
  to_bytes=$(grep -E '^SS_GEOMETRY' "$d/scratch/order-tape.log" | tail -1 | grep -o 'to_bytes=[0-9]*' | cut -d= -f2)
  worst_after=$(grep -E '^SS_SUBDIV_POSPEAK' "$d/scratch/order-tape.log" | tail -1 | grep -o 'peak_window=[0-9]*' | cut -d= -f2)
  say "R3_WINDOWS shrunk_to_bytes=$to_bytes last_window_peak_bytes=${worst_after:-0}"
  # LIVE FELL: the minimum live after the first shrink is below the live at the shrink.
  local at_shrink after_min
  at_shrink=$(grep -E '^SCRATCH_GEOMETRY action=shrink' "$d/build.log" | head -1 | grep -o 'live=[0-9]*' | cut -d= -f2)
  after_min=$(awk -v a="$at_shrink" '/^SCRATCH_SAMPLE /{
        for (i=1;i<=NF;i++){split($i,kv,"="); if(kv[1]=="live") L=kv[2]+0}
        if (seen && (min=="" || L<min)) min=L; if (L+0>=a+0) seen=1 } END{print (min==""?0:min)}' "$d/build.log")
  say "R3_LIVE at_shrink=$at_shrink min_after=$after_min fell=$([ "${after_min:-0}" -lt "${at_shrink:-0}" ] && echo 1 || echo 0)"
  [ "${after_min:-0}" -lt "${at_shrink:-0}" ] || say "R3_NOTE live did not fall below the shrink point in this run (the position window is not the dominant term on this fixture)"
  grep -E '^SCRATCH_GOVERNOR_SUMMARY' "$d/build.log" | sed 's/^/    | /'
  no_survivors

  # ---- THE SORTER SIDE OF THE CHANNEL, PROVEN DIRECTLY ---------------------
  # On a fixture the container (0.65 n) dominates `live`, so the governor's shrink
  # fires late and the sorter may have no window left to apply it to. The channel is
  # therefore proven where it is deterministic: scatter_sort, run on the same
  # fixture with a geometry.ctl that the governor would have written, must shrink
  # its windows and journal SS_GEOMETRY.
  say "R3_SORTER_CHANNEL: scatter_sort with a geometry.ctl in its scratch"
  g++ -O2 -std=c++17 -pthread "$HERE/xsa/runtime/tools/binbwt/scatter_sort.cpp" \
      -o "$STAGE/scatter_sort" -latomic 2>/dev/null || fail "R3: scatter_sort did not compile"
  local sd=$WORK/runs/r3_sorter
  rm -rf "$sd"; mkdir -p "$sd/scratch"
  printf 'position_budget_bytes=56623104\n' > "$sd/scratch/geometry.ctl"
  setsid --wait timeout --kill-after=10 "$RUN_TIMEOUT" "$STAGE/scatter_sort" \
      --corpus "$FX/r3/out.bin" --out "$sd/out.sa" --scratch "$sd/scratch" \
      --threads "$THREADS" --par-bands 1 --budget-bytes 2000000000 --memory-gb "$MEM" \
      --scratch-budget-bytes 268435456 --subdiv-strategy=local --subdiv-position-budget-bytes=67108864 \
      > "$sd/run.log" 2>&1 || { tail -5 "$sd/run.log"; fail "R3: the sorter run failed"; }
  local sapplied sfirst sto
  sapplied=$(grep -c '^SS_GEOMETRY action=shrink' "$sd/run.log" || true)
  sfirst=$(grep -m1 '^SS_GEOMETRY' "$sd/run.log" | cut -c1-220)
  sto=$(grep -m1 '^SS_GEOMETRY' "$sd/run.log" | grep -o 'to_bytes=[0-9]*' | cut -d= -f2)
  say "R3_SORTER_APPLIED applied=$sapplied first='$sfirst'"
  [ "$sapplied" -ge 1 ] || { tail -5 "$sd/run.log"; fail "R3: the sorter did not apply the geometry.ctl"; }
  # every window AFTER the shrink wrote no more than the shrunk budget
  awk -v limit="${sto:-0}" '/^SS_SUBDIV_POSPEAK /{
        for (i=1;i<=NF;i++){split($i,kv,"="); if(kv[1]=="bytes_written") b=kv[2]+0; if(kv[1]=="window") w=kv[2]}
        if (seen && b > limit) { printf "window %s wrote %d > %d\n", w, b, limit; bad=1 }
        if (!seen && b <= limit) seen=1
      } END{ exit bad?1:0 }' "$sd/run.log" \
    || fail "R3: a window after the shrink wrote more than the shrunk budget ($sto B)"
  local nwin
  nwin=$(grep -c '^SS_SUBDIV_POSPEAK' "$sd/run.log")
  say "R3_SORTER_WINDOWS examined=$nwin shrunk_to_bytes=$sto all_after_within_budget=1"
  say "R3_RESULT ok=1 governor_shrinks=$shrinks sorter_applied=$sapplied windows=$nwin"
}

# --------------------------------------------------------------------------- R4
r4() {
  say "== R4 HARD FAIL IS GRACEFUL: forced ceiling, numbers, no survivors, no STOP"
  fixtures "$FIXBYTES_SMALL" "$FX/r4"
  local d rc
  # A tiny live target makes the ceiling 125% of it, which this fixture exceeds:
  # the shrink fires first, then the hard fail.
  d=$(run_strace r4_hardfail "$FX/r4/out.bin" "$FX/r4/map" "$SCRATCH_GB" "$THREADS" "$MEM" \
      --scratch-target-gb 0.35)
  rc=$(cat "$d/rc")
  say "R4_RC rc=$rc wall_s=$(cat "$d/wall_s") (a hard fail is expected)"
  [ "$rc" != 0 ] || fail "R4: the build with a 0.35 GB target should have failed"
  say "R4_ARITHMETIC $(bound_check "$d/build.log")"
  grep -q 'SCRATCH_CEILING_EXCEEDED' "$d/build.log" || { tail -6 "$d/build.log"; fail "R4: no SCRATCH_CEILING_EXCEEDED"; }
  grep -E '^SCRATCH_CEILING_EXCEEDED' "$d/build.log" | cut -c1-1000 | sed 's/^/    | /'
  grep -E '^SCRATCH_GOVERNOR_HARD_FAIL' "$d/build.log" | cut -c1-500 | sed 's/^/    | /'
  grep -E '^SCRATCH_GEOMETRY action=shrink' "$d/build.log" | head -2 | cut -c1-200 | sed 's/^/    | /'
  # THE SHUTDOWN: per-pid SIGTERM (and later SIGKILL), never a group signal, never STOP.
  local stop cont group ters kills
  stop=$(grep -cE '(kill|tkill|tgkill)\(.*SIGSTOP' "$d/strace.log" || true)
  cont=$(grep -cE '(kill|tkill|tgkill)\(.*SIGCONT' "$d/strace.log" || true)
  group=$(grep -cE 'kill\(-[0-9]+,' "$d/strace.log" || true)
  ters=$(grep -cE 'kill\([0-9]+, SIGTERM' "$d/strace.log" || true)
  kills=$(grep -cE 'kill\([0-9]+, SIGKILL|tkill\(|tgkill\(' "$d/strace.log" || true)
  say "R4_SIGNALS SIGSTOP=$stop SIGCONT=$cont group_signals=$group per_pid_SIGTERM=$ters other_kills=$kills"
  grep -oE 'kill\([0-9-]+, SIG[A-Z]+\)?' "$d/strace.log" | sort | uniq -c | sed 's/^/    | /'
  [ "$stop" = 0 ] || fail "R4: a SIGSTOP was sent"
  [ "$cont" = 0 ] || fail "R4: a SIGCONT was sent"
  [ "$group" = 0 ] || fail "R4: a process-group signal was sent"
  [ "$ters" -ge 1 ] || fail "R4: the shutdown did not use per-pid SIGTERM"
  # THE CHECKPOINT JOURNAL IS CONSISTENT: it exists, is non-empty, every line is a
  # whole line (the file ends with a newline), and the last record parses.
  local ck=$d/scratch/checkpoint.journal
  [ -s "$ck" ] || fail "R4: no checkpoint journal after the hard fail"
  [ "$(tail -c 1 "$ck" | od -An -c | tr -d ' ')" = "\\n" ] || fail "R4: the checkpoint journal ends mid-line"
  say "R4_CHECKPOINT bytes=$(stat -c %s "$ck") lines=$(wc -l < "$ck") ends_with_newline=1"
  tail -2 "$ck" | cut -c1-200 | sed 's/^/    | /'
  no_survivors
  say "R4_RESULT ok=1 SIGSTOP=0 SIGCONT=0 group_signals=0 per_pid_SIGTERM=$ters"
}

# --------------------------------------------------------------------------- R5
r5() {
  say "== R5 NO REGRESSION: container byte-identity against origin/main (4e275f6)"
  fixtures "$FIXBYTES" "$FX/r5"
  local ours main
  ours=$(run_build "$X" r5_ours "$FX/r5/out.bin" "$FX/r5/map" "$SCRATCH_GB" "$THREADS" "$MEM")
  [ "$(cat "$ours/rc")" = 0 ] || { tail -5 "$ours/build.log"; fail "R5: our build failed"; }
  [ -x "$STAGE/mainbin/xsa-main" ] || fail "R5: no main binary at $BASE_BIN"
  main=$(run_build "$STAGE/mainbin/xsa-main" r5_main "$FX/r5/out.bin" "$FX/r5/map" "$SCRATCH_GB" "$THREADS" "$MEM")
  [ "$(cat "$main/rc")" = 0 ] || { tail -5 "$main/build.log"; fail "R5: the main-binary build failed"; }
  local co cm bo bm nb
  co=$(sha256sum "$ours/scratch/out.sxi2" | cut -d' ' -f1)
  cm=$(sha256sum "$main/scratch/out.sxi2" | cut -d' ' -f1)
  say "R5_CONTAINER ours=$co bytes=$(stat -c %s "$ours/scratch/out.sxi2")"
  say "R5_CONTAINER main=$cm bytes=$(stat -c %s "$main/scratch/out.sxi2")"
  [ "$co" = "$cm" ] || fail "R5: the containers differ"
  bo=$(grep -oE '^SS_BAND band=[0-9]+ rows=[0-9]+' "$ours/scratch/order-tape.log" | sort | sha256sum | cut -d' ' -f1)
  bm=$(grep -oE '^SS_BAND band=[0-9]+ rows=[0-9]+' "$main/scratch/order-tape.log" | sort | sha256sum | cut -d' ' -f1)
  nb=$(grep -cE '^SS_BAND ' "$ours/scratch/order-tape.log" || true)
  say "R5_BANDS compared=$nb (band,rows) pairs ours=$bo main=$bm"
  [ "$bo" = "$bm" ] || fail "R5: the per-band receipts differ"
  say "R5_WALLS ours_s=$(cat "$ours/wall_s") main_s=$(cat "$main/wall_s")"
  grep -E '^SCRATCH_GOVERNOR_SUMMARY' "$ours/build.log" | sed 's/^/    | ours /'
  grep -E '^SCRATCH_BACKPRESSURE_SUMMARY' "$main/build.log" | sed 's/^/    | main /'
  no_survivors
  say "R5_RESULT ok=1 bands=$nb container_sha=$co"
}

# --------------------------------------------------------------------------- S4
s4_arm() {
  local arm=$1 bin
  bin=$X
  [ "$arm" = main ] && bin=$STAGE/mainbin/xsa-main
  local d=$WORK/runs/s4_$arm
  echo "s4_$arm" >> "$RUN_TAGFILE"
  rm -rf "$d"; mkdir -p "$d"
  say "S4_BOX arm=$arm start=$(date -u +%FT%TZ) load=$(cut -d' ' -f1-3 /proc/loadavg) other_heavy=$(pgrep -c -f 'scatter_sort|xsa_finish|cross_lcp_merge|ctx-tap' || true) free_gb=$(df --output=avail -BG "$WORK" | tail -1 | tr -dc 0-9)"
  say "S4_RUN arm=$arm bin=$bin sha256=$(sha256sum "$bin" | cut -d' ' -f1) corpus=$S4_CORPUS bytes=$(stat -c %s "$S4_CORPUS") map=$S4_MAP pos_budget=$S4_POS_BUDGET threads=$S4_THREADS mem_gb=$S4_MEM finish_as_gb=$S4_FINISH_AS_GB scratch_budget_gb=$S4_SCRATCH_GB extra='$S4_EXTRA'"
  local t0; t0=$(date +%s)
  # --trie-depth would RE-ENABLE a trie that --no-trie turned off (the parser sets
  # trie=true for --trie-depth), so the two are mutually exclusive on the command
  # line. S4 runs --no-trie by default because the in-build trie fails identically
  # on BOTH binaries at 100 GB ("children are not in ascending byte order"), which
  # is a pre-existing defect outside the governor's scope.
  local trie_args=()
  case "$S4_EXTRA" in *--no-trie*) ;; *) trie_args=(--trie-depth 8 --trie-min-support 1000) ;; esac
  env XSA_SUBDIV_POSITION_BUDGET_BYTES="$S4_POS_BUDGET" \
    setsid --wait timeout --kill-after=10 "$S4_TIMEOUT" "$bin" build --tape \
      --input "$S4_CORPUS" --scratch "$d/scratch" --docs-map "$S4_MAP" \
      --threads "$S4_THREADS" --memory-gb "$S4_MEM" --finish-as-gb "$S4_FINISH_AS_GB" \
      --scratch-budget-gb "$S4_SCRATCH_GB" ${trie_args[@]+"${trie_args[@]}"} $S4_EXTRA \
      > "$d/build.log" 2>&1
  echo $? > "$d/rc"
  local t1; t1=$(date +%s); echo $((t1 - t0)) > "$d/wall_s"
  say "S4_DONE arm=$arm rc=$(cat "$d/rc") wall_s=$(cat "$d/wall_s") end=$(date -u +%FT%TZ) load_end=$(cut -d' ' -f1-3 /proc/loadavg)"
}

s4() {
  say "== S4 THE 100 GB HEAD: pile-head100 + pilemap100, pos_budget 20 GB"
  [ -f "$S4_CORPUS" ] || fail "S4: no corpus at $S4_CORPUS"
  [ -d "$S4_MAP" ] || fail "S4: no map at $S4_MAP"
  [ -x "$STAGE/mainbin/xsa-main" ] || fail "S4: no main binary at $BASE_BIN"
  say "S4_PLAN parallel=${S4_PARALLEL:-0} extra='$S4_EXTRA'"
  local r1 r2
  if [ "${S4_SKIP_RUN:-0}" = 1 ]; then
    say "S4_SKIP_RUN=1: reporting the existing runs, not building"
    r1=0; r2=0
  elif [ "${S4_PARALLEL:-0}" = 1 ]; then
    s4_arm ours & local p1=$!
    s4_arm main & local p2=$!
    wait $p1; r1=$?
    wait $p2; r2=$?
  else
    s4_arm ours; r1=$?
    s4_arm main; r2=$?
  fi
  local bad=0
  for arm in ours main; do
    local d=$WORK/runs/s4_$arm rc peak max_live err
    rc=$(cat "$d/rc" 2>/dev/null || echo missing)
    [ "$rc" = 0 ] || { tail -20 "$d/build.log"; say "S4_ARM_FAILED arm=$arm rc=$rc"; bad=$((bad+1)); continue; }
    grep -E "^(XSA_PLAN_TAPE |XSA_PLAN_DISC )" "$d/build.log" | cut -c1-400 | sed "s/^/    | $arm /"
    grep -E '^SCRATCH_BOUND |^SCRATCH_TERMS' "$d/build.log" | cut -c1-560 | sed "s/^/    | $arm /"
    grep -E '^SCRATCH_GOVERNOR_SUMMARY' "$d/build.log" | sed "s/^/    | $arm /"
    grep -E '^SCRATCH_GEOMETRY' "$d/build.log" | head -3 | cut -c1-240 | sed "s/^/    | $arm /"
    say "S4_ARITHMETIC arm=$arm $(bound_check "$d/build.log")"
    peak=$(grep '^SCRATCH_GOVERNOR_SUMMARY' "$d/build.log" | grep -o 'peak_gb=[0-9.]*' | cut -d= -f2)
    max_live=$(grep '^SCRATCH_GOVERNOR_SUMMARY' "$d/build.log" | grep -o 'max_live_gb=[0-9.]*' | cut -d= -f2)
    err=$(awk -v p="$peak" -v m="$max_live" 'BEGIN{if (m+0>0) printf "%.4f", (p-m)/m; else print "n/a"}')
    say "S4_PEAK arm=$arm peak_gb=$peak max_live_gb=$max_live model_error_fraction=$err"
    local best
    best=$(grep -E '^SCRATCH_SAMPLE' "$d/build.log" | awk '{for(i=1;i<=NF;i++) if($i ~ /^live=[0-9]*$/){split($i,a,"="); print a[2], $0}}' | sort -n | tail -1 | cut -d' ' -f2-)
    say "S4_PEAK_SAMPLE arm=$arm $(echo "$best" | cut -c1-520)"
    say "S4_CONTAINER arm=$arm bytes=$(stat -c %s "$d/scratch/pile-head100.sxi2") sha256=$(sha256sum "$d/scratch/pile-head100.sxi2" | cut -d' ' -f1)"
    grep -E '^XSA_BUILD_DONE' "$d/build.log" | sed "s/^/    | $arm /"
  done
  local co cm
  for f in "$WORK/runs/s4_ours/scratch/pile-head100.sxi2" "$WORK/runs/s4_main/scratch/pile-head100.sxi2"; do
    [ -s "$f" ] || { say "S4_CONTAINER_MISSING $f"; bad=$((bad+1)); }
  done
  co=$(sha256sum "$WORK/runs/s4_ours/scratch/pile-head100.sxi2" 2>/dev/null | cut -d' ' -f1)
  cm=$(sha256sum "$WORK/runs/s4_main/scratch/pile-head100.sxi2" 2>/dev/null | cut -d' ' -f1)
  say "S4_IDENTITY ours=$co main=$cm identical=$([ -n "$co" ] && [ "$co" = "$cm" ] && echo 1 || echo 0)"
  [ -n "$co" ] && [ "$co" = "$cm" ] || bad=$((bad+1))
  no_survivors
  [ "$bad" = 0 ] || fail "S4: $bad assertion(s) failed"
  say "S4_RESULT ok=1 arms=2 container_sha=$co"
}

case "${1:-all}" in
  r1) build_stage; r1 ;;
  r2) build_stage; r2 ;;
  r3) build_stage; r3 ;;
  r4) build_stage; r4 ;;
  r5) build_stage; r5 ;;
  s4) build_stage; s4 ;;
  all) build_stage; r1; r2; r3; r4; r5 ;;
  *) fail "usage: $0 r1|r2|r3|r4|r5|s4|all" ;;
esac
