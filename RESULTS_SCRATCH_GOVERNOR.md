# The scratch governor: the ceiling arithmetic, the box-freeze, and the numbers

Branch `fix/scratch-ceiling` (from `origin/main` `4e275f6`). Three incidents, one
cause: **the governor's ceiling was not derived from what the build writes, and its
pause was not a pause.**

```
xsa/src/scratchgov.rs                     the ONE term table, the ONE sampler, the pure ceiling
xsa/src/chain.rs                          the governor: announce/sample/enforce, kill(2) by syscall,
                                          --scratch-reserve-gb, USAGE
gates/scratch_governor_gates.sh           S1, S2, S3, S4, S6
```

---

## 0. The three incidents

### A. The monitor could not act (live 8,189 GB, `paused=0`)

```
SCRATCH_SAMPLE live_gb=8188 ... budget_gb=250 paused=0
waves-tape/subdiv_pos   7.6 TB        (four windows x 2 TB)
filesystem              100 % full    -> scatter_sort: write position file
```

The sampler walked the whole tree, but the only term the response knew how to
price was the tape budget; the position window — the one scratch term **no
consumer can drain**, because the sorter frees it itself — could not stop the box.
The stall path then *ratcheted the pause threshold upward* so the build would not
deadlock, and the box filled. A monitor that cannot act is not a monitor.

*(The write-through cause — every window's position files staying on disc until
the end — is fixed in `RESULTS_SUBDIV_DISK.md`. This document is about the
governor, which still took no action at 8 TB.)*

### B. The governor acted, on a ceiling that did not price the build

```
SCRATCH_GOVERNOR_CEILING_EXCEEDED live_bytes=2,324,826,972,160
                                 ceiling_bytes=2,322,122,547,200
journaled formula: pos_budget_gb 1862.645 + budget_gb 250 + margin 50  ->  2162.645 GiB
```

Over by 2.7 GB, at a number the operator cannot predict, because **the merged
fragment family (`merged/frag.pfck2` + `merged/frag.chi-bitmap`, 443 GB — which is
exactly `plan::V2_PFCK_PER_N` x n for that corpus) and the container partial were
not in the formula at all.** The knobs bounded what the pipeline *intended*; the
ceiling was supposed to bound what the *box* could afford, and it did neither.

### C. The pause path was the box-freeze (2026-11-14 09:38:45)

The measured, not the suspected, root cause of the freezes (2026-11-09 ~23:00 and
2026-11-14 09:38:45):

```
procps-ng 4.0.4 `kill -<SIG> -<pgid>` keeps only the FIRST DIGIT of a negative pid spec.
  strace -e kill /usr/bin/kill -0 -1257815   ->  kill(-1, 0)      # EVERY process of the user
  strace -e kill /usr/bin/kill -0 -2449516   ->  kill(-2, 0)
  strace -e kill /usr/bin/kill -0 -9876543   ->  kill(-9, 0)
```

So the governor's **pause** — the safety mechanism — issued `kill(-1, SIGSTOP)` and
stopped **every process of the user**, and the ceiling **kill** in the same spelling
was a `kill(-1, SIGKILL)` waiting for the first overrun. The pile build's own
governor crossed its 250 GB pause threshold at `live_gb=251.115`
(`PILE/xsa-build.log`, last line, 09:38:46.698) and froze the box, its own phases
included; PILE then stood static at 269,682,057,216 B with all nine of its processes
in state `T`.

**The fix: the governor calls `kill(2)` directly** with `-(pgid as i64) as i32`
(exactly what `RunningPhase::terminate` already did), `group_target()` refuses
pgid 0 and 1 (`kill(0)` is the caller's group, `kill(-1)` is every process of the
user), and the signal map is closed to STOP/CONT/KILL. Gate S6 pins it.

---

## 1. Account every live term: one table

`xsa/src/scratchgov.rs` holds **one** classifier and **one** sampler; the journal's
terms, the live total and the ceiling's arithmetic all come from the same walk.

| term | rule (the paths it owns) | why it must be in the ceiling |
|---|---|---|
| `term_pos` | `waves-tape/subdiv_pos/**` (tape), `*/binbwt-windows/**` (bin) | the one term **no consumer can drain**; the sorter frees it itself (incident A) |
| `term_bands` | `bands-tape/**`, `waves-tape/**` (not `subdiv_pos`) | the band window: the term the tape budget sizes (60 %) |
| `term_merged` | `merged/**` except `merged/frag.pieces/**`: `frag.rlebwt`, `frag.pfck2`, `frag.chi-bitmap`, `frag.bits-*`, `frag.finish-spool` | **incident B's missing 443 GB**; not bounded by the tape budget |
| `term_pieces` | `merged/frag.pieces/**` | the in-flight piece window (the other 40 % of the budget) |
| `term_container` | `X.sxi2`, `X.sxi2.partial`, `X.sxi2.part` at the scratch root | the PRODUCT on the same filesystem: it fills the disc exactly as fast |
| `term_other` | `*.log`, `*.time`, `remap-256.bin`, `checkpoint.journal`, `trie-m20.bin`(+`.partial`/`.part`), `stop.requested`, `xsa-build.log`, and **anything the table does not name** | the named small files, plus an explicit home for a new family |

Rules that make the sum equal `du` (both are gates):

* **allocated bytes** (`st_blocks x 512`), never apparent size (the chi bitmap is a
  sparse n/8 mapping);
* **hard links counted once** (dev+ino), exactly as `du` counts them: the finish
  links the container beside itself for the in-place member rewrite, and counting
  both names broke the identity by a whole container (measured: 95 % delta);
* an **unnamed** path is still counted (folded into `term_other`, so `live` stays
  the whole tree) but it is journaled by name as `SCRATCH_LEDGER_UNKNOWN` and
  `unknown_paths` rises. `unknown_paths=0` is a gate.

The sampler is the only one in the build (`scratch_bytes_excluding` is gone).

---

## 2. The ceiling: before and after

**Before** (`chain.rs`, the formula incident B was killed by):

```
ceiling = tape_budget + pos_budget + 50 GB
```

**After** (`scratchgov::derive_ceiling`, one pure function):

```
fs_arm   = free_at_start - reserve            # statvfs at plan time, f_bavail x f_frsize
knob_arm = sum(term budgets) + measured_slack
ceiling  = min(fs_arm, knob_arm)
arm      = filesystem | knobs                 # journaled: which one won
pause    = 90 % of min(ceiling, sum(term budgets))     # the intent bound
resume   = 85 % of the same basis
```

* `reserve` = `--scratch-reserve-gb`, DEFAULT `max(10 % of the filesystem, 500 GB)`,
  clamped to half the filesystem (`reserve_clamped=1` when it bites) so a small
  scratch device cannot produce a ceiling of zero;
* `measured_slack` = `max(8 GB, 5 % of the term sum)` — the allowance for the term
  budgets' own estimation error (the plan's per-n rates are measured, the wave is
  sized from a sample estimate). It is added to the **knobs** arm only: it can
  never widen the filesystem arm, so the box keeps its veto.

The knobs arm, term by term, and where each number comes from (journaled as
`SCRATCH_TERMS ... budget_sources=`):

| term budget | default | source |
|---|---|---|
| `pos` | `max(256 MB, budget/5)` | `XSA_SUBDIV_POSITION_BUDGET_BYTES` — **the same knob the sorter reads** |
| `bands` | 60 % of `--scratch-budget-gb` | `plan::TAPE_BAND_SHARE_PCT` |
| `pieces` | 40 % of `--scratch-budget-gb` | `plan::TAPE_PIECE_SHARE_PCT` |
| `merged` | tape: `(0.364 + 0.283) n`; bin: `1.975 n` | the plan's measured per-n rates (`V2_PFCK_PER_N` + `V2_CHI_PER_N`; `V2_TREE_PEAK_PER_N` on the bin route, which also writes `frag.rlebwt`) |
| `container` | `1.5 x 0.468 n` | `V2_CONTAINER_PER_N` x 1.5 for the optional members (m14 costs 0.256 n, the trie and m15 ride along) |
| `other` | 50 GB | `GOVERNOR_OTHER_BUDGET_BYTES` (the sorter's own scratch margin; the in-build m20 payload is 0.023 n at 100 GB) |

### The arithmetic on the incident-B numbers (gate S5, a unit test)

```
$ cargo test --release scratchgov -- --nocapture
INCIDENT_B old_ceiling_bytes=2322122547200 live_actual_bytes=2324826972160
           new_ceiling_bytes=2937679515745 new_ceiling_gb=2735.927 arm=knobs
           knob_arm_gb=2735.927 fs_arm_gb=6800.000 slack_gb=130.282
INCIDENT_B pause_at_bytes=2518011013491 pause_at_gb=2345.080
INCIDENT_B recommended pos_gb=20 ceiling_gb=801.150 pause_gb=686.700 live_actual_gb=2165.164
```

* the old ceiling (2162.6 GiB) was **below** the live number that occurred; the new
  one (2735.9 GiB) is **above** it — the fix predicts the overrun instead of being
  surprised by it;
* with the incident's own knobs the pause sits at 2345.1 GiB, i.e. below the new
  ceiling and available *before* it (the response ordering);
* with the **recommended** knobs (pos 20 GB, not 2 TB) the ceiling is **801 GiB** —
  the overrun that killed the pile at 2324.8 GB is caught at 801 GB, and the pause
  lands at 687 GB.

### The incident-A numbers (a unit test)

```
INCIDENT_A ceiling_gb=3334.427 arm=knobs fs_arm_gb=6800.000 knob_arm_gb=3334.427 reserve_gb=1400.000
INCIDENT_A small_ceiling_gb=592.000 arm=knobs
```

The position budget is 2 TB, so on an 8.2 TB box the **filesystem** arm
(8200 − 1400 = 6800 GiB) is what would have stopped the 8,189 GB run — and when the
declared intent is smaller than the box, the **knobs** arm wins (592 GiB, exactly
`20 + 150 + 100 + 197 + 47 + 50 + 28`). A ceiling is the smaller of the two, never
the sum of knobs.

---

## 3. Act early, kill only on a sustained overrun

```
overrun: pause the producers (SIGSTOP on their process groups) -> journal SCRATCH_BACKPRESSURE
         state=pause why=over_ceiling
         every sample past the ceiling: SCRATCH_OVERRUN samples=n action=drain|kill
         sample 4 (kill_after_samples=3, i.e. 6 s at the 2 s interval): KILL + fail with the
         term breakdown, the arm, free_at_start, reserve, fs_arm, knob_arm and the levers
```

`SCRATCH_OVERRUN samples=` resets the moment live falls back inside the ceiling, so
a drain that is working is never killed. The pause threshold may still ratchet up
(the residual position window is scratch no consumer can release) but **never past
the ceiling**, and the ratchet is journaled with `why=` and `capped=`.

---

## 4. The gates (verbatim, with N)

Binary under test `sha256=10832078b5d8c7b3833e797def687127495db9e77c454a573c226cc0137fdc71`
(branch `fix/scratch-ceiling`); reference binary `sha256=85924aeb…d16420`, built once
from origin/main at the branch point (`git archive 4e275f6`).

### S1 TERM COVERAGE — sum(terms) == `du` within 1 %, unknown = 0

512 MB fixture (`gates/forward_only_fixture.c`), `--scratch-budget-gb 0.5`,
`XSA_SCRATCH_AUDIT=1` (the product's own audit: `du` on the same tree, bracketed by
two ledger samples, on every sample at which two consecutive samples agree — a
moving tree cannot be read consistently by `du`).

```
S1_ARM arm=auto rc=0 wall_s=276
S1_SAMPLES arm=auto compared=9 du_complete=9 over_limit=0 worst_complete_delta_pct=0.1259 unknown_path_samples=0
SCRATCH_AUDIT_SUMMARY samples=9 complete=9 worst_delta_pct=0.1259 partial=0 partial_worst_pct=0.0000
                      unknown_paths_seen=0 du_failures=0 quiescent_only=1
SCRATCH_BACKPRESSURE_SUMMARY budget_gb=0.500 peak_gb=4.138 ceiling_gb=59.425 arm=knobs ...
   peak_terms=term_pos=0,term_bands=239341568,term_merged=270610432,term_pieces=62619648,
              term_container=4294836224,term_other=132071424
S1_RESULT arms=1 complete_samples_compared=9 failures=0
SCRATCH_GOVERNOR_NO_SURVIVORS checked=1 runs (own scratch dirs only)
```

Worst delta over 9 complete comparisons **0.1259 %** (limit 1 %), unknown paths **0**
in all 148 `SCRATCH_SAMPLE` lines of that arm. A full-load repeat of the same arm
gave `compared=31 complete=31 worst_complete_delta_pct=0.1322 unknown=0`.

`term_pos` is 0 on this arm because on a 0.5 GiB budget the sorter picks the global
strategy (no position window); the position term is exercised at scale (S4) and by
the `classify()` unit test. `S1_LOCAL=1` runs the sorter's debug arm
`XSA_SUBDIV_STRATEGY=local`, which does write `subdiv_pos` — one such run measured
`peak_terms=term_pos=64999424` with the same 0.08 % audit delta; a later attempt on
the same fixture crawled in the finish's LCP-repair pass and was stopped by its
timeout, so the arm is opt-in and the *scale* evidence is S4.

### S2 CEILING PREDICTABLE — pause first, kill only on a sustained overrun

256 MB fixture. Comfortable arm (`ceiling ~59 GiB`, the knobs arm):

```
S2_COMFORTABLE rc=0 wall_s=128 peak_gb=2.060 ceiling_gb=59.087 pauses=0 overruns=0
S2_ARITHMETIC ok=1 ceiling=63444462639 pause_at=49369075236 free_at_start=6478254415872
   reserve=1536085441740 term_sum=54854528047 slack=8589934592 fs_arm=4942168974132
   knob_arm=63444462639 arm=knobs recomputed_ceiling=63444462639 recomputed_pause_at=49369075236
```

Forced arm (`--scratch-reserve-gb` pushed so the FILESYSTEM arm binds below the
measured peak) — recomputed from the journal's own numbers, `ok=1`:

```
S2_ARITHMETIC attempt=1 ok=1 ceiling=774143734 pause_at=696729357
   free_at_start=6476550701056 reserve=6475776557322 fs_arm=774143734 arm=filesystem
SCRATCH_BACKPRESSURE state=pause live=813899776 ceiling=774143734 over_by=39756042
   why=over_ceiling action=producer_stopped consumer=drains
SCRATCH_OVERRUN samples=1 live=813899776 ceiling=774143734 action=drain
SCRATCH_OVERRUN samples=2 live=807600128 ceiling=774143734 action=drain
SCRATCH_OVERRUN samples=3 live=807600128 ceiling=774143734 action=drain
SCRATCH_OVERRUN samples=4 live=807600128 ceiling=774143734 action=kill
SCRATCH_GOVERNOR signal=KILL phase=order-tape pgid=3238134
SCRATCH_GOVERNOR signal=KILL phase=emit-tape pgid=3238135
S2_ORDER pause_events=1 overrun_samples=4 (kill_after_samples=3 -> 4 overrun samples)
S2_LINE_ORDER pause=99 overrun=100 kill=107
S2_RESULT ok=1
```

The ceiling the governor reports **is** the journaled arithmetic
(`free_at_start − reserve` = 774,143,734 B, recomputed to the byte). The pause is
the first response (line 99), the overrun is counted (100), and the kill comes only
on the 4th sustained overrun sample (107) — 8 s after the pause, not at the first
byte over the line.

### S3 NO REGRESSION — byte-identical container, N bands compared

512 MB fixture, identical flags, our binary vs origin/main `4e275f6`:

```
S3_CONTAINER ours=8142cbbc85fe96bc3a41206fdceaeabdaf7514a3f0e4faea32e8f861a63ee7e3 bytes=3422870896
S3_CONTAINER main=8142cbbc85fe96bc3a41206fdceaeabdaf7514a3f0e4faea32e8f861a63ee7e3 bytes=3422870896
S3_BANDS compared=5519 (band,rows) pairs ours_receipts_sha=108dd516…dc07c6 main_receipts_sha=108dd516…dc07c6
S3_WALLS ours_s=275 main_s=271
S3_RESULT ok=1 bands=5519 container_sha=8142cbbc…ee7e3
```

5519 bands compared (band id + row count, sorted: the writer slots emit
concurrently, so the *set* is the contract). The two governor summaries are the
incident-B story in one line each:

```
ours: peak_gb=4.138 ... accounting=every_byte_under_scratch_incl_container
main: peak_gb=0.492 ... accounting=scratch_excludes_container
```

the same build, the same 4.0 GB container: the old accounting under-reported live
scratch by the whole product.

### S4 THE 100 GB HEAD — see §5 (long run, both arms, identical flags)

### S6 THE SIGNAL TARGET — sign-correct group id, never `kill(-1)`, never `kill(1)`

A real forced-ceiling product run under
`strace -f -qq -e trace=kill,execve`:

```
S6_RUN attempt=1 reserve_gb=6031.035359 target_ceiling_gb=0.721000
S6_SYSCALLS kill_calls=8 negative_pid_calls=8
    |       2 kill(-3065805, SIGKILL)
    |       1 kill(-3065805, SIGSTOP)
    |       1 kill(-3065806, SIGKILL)
    |       1 kill(-3065809, SIGKILL)
S6_TRUNCATED_OR_SPECIAL_TARGETS ''
S6_JOURNAL_SIGNALS 'KILL,3065804 KILL,3065805 STOP,3065804 STOP,3065805'
S6_JOURNALED_SIGNALS_VERIFIED 4 (each one syscall, sign-correct, whole group id)
S6_KILL1_EXECVE 0 S6_SHELL_KILL_EXECVE 0
S6_RESULT ok=1 sign_correct_group_ids=1 kill_minus_one=0 kill1_execve=0
```

Every journaled `signal=STOP|KILL phase=… pgid=N` has exactly one `kill(-N, SIG…)`
syscall with the **whole** group id; no `kill(0)`, no `kill(-1)`, no truncated pid;
`kill(1)` is never executed and no shell is used as its vehicle. The unit test
`chain::tests::signal_group_target_is_sign_correct_and_never_special` covers the
dangerous ranges without signalling anything:

```
SIGNAL_TARGET examined=9 special_refused=2 signals=3
```
(pgids 2, 3, 9, 999999, 1000000, **1257815**, 1999999, 2000000, 9876543 -> each maps
to `-(pgid)`, never `-1`; pgid 0 and 1 refused; only STOP/CONT/KILL accepted.)

---

## 5. S4: the 100 GB head, both arms

<!-- S4-TABLE -->

---

## 6. The knobs (and their help text)

| knob | default | what it does |
|---|---|---|
| `--scratch-budget-gb N` | derived (`min(0.8 × free, 2 × band window)`) | sizes the band window (60 %) and the piece window (40 %); it is the `term_bands + term_pieces` arm of the ceiling |
| `--scratch-reserve-gb F` | `max(10 % of the filesystem, 500 GB)`, clamped to half | **NEW**: the ceiling's filesystem arm is `free_at_start − reserve`; decimal GB so a fixture can push the ceiling to a few tens of MB |
| `XSA_SUBDIV_POSITION_BUDGET_BYTES` | `max(256 MB, budget/5)` | the position window (`term_pos`), read by the sorter **and** the governor (they can never disagree) |
| `XSA_SCRATCH_AUDIT=1` | off | the product's own S1 oracle: `SCRATCH_AUDIT_TERMS` compares the journaled term sum against `du` on the same tree |

Journal lines: `SCRATCH_GOVERNOR` (every input of the ceiling + which arm won),
`SCRATCH_TERMS` (the term budgets and their sources), `SCRATCH_TERM` (the rule of
each term, once), `SCRATCH_SAMPLE` (`term_pos= term_bands= term_merged= term_pieces=
term_container= term_other= live= ceiling= arm=`), `SCRATCH_LEDGER_UNKNOWN`,
`SCRATCH_BACKPRESSURE`, `SCRATCH_OVERRUN`, `SCRATCH_GOVERNOR ratchet`,
`SCRATCH_AUDIT_TERMS`, `SCRATCH_AUDIT_SUMMARY`, `SCRATCH_BACKPRESSURE_SUMMARY`
(with `peak_terms=`).

---

## 7. The recommended knobs for the next pile relaunch

Pile: `pile.txt` N = 1,307,910,802,540; scratch filesystem 14 TB.

```
XSA_SUBDIV_POSITION_BUDGET_BYTES=20000000000     # 20 GB, freed per window
--memory-gb 96 --threads 96                      # the RAM law's envelope
--finish-as-gb 400
--scratch-budget-gb 100                          # 100 GB: feasible at this envelope (below)
--scratch-reserve-gb <default: 1430 GB here>     # 10 % of the 14 TB scratch
docs-map pilemap_full (exact-corpus map)
```

The ceiling those numbers derive (the same arithmetic the journal prints):

```
term_sum = 20(pos) + 60(bands) + 40(pieces) + 846(merged 0.647n) + 918(container 0.702n) + 50(other)
         = 1,934 GB
slack    = 5 % -> 97 GB          knob_arm = 2,031 GB
fs_arm   = free_at_start - 1,430 GB   (8.2 TB free -> 6,770 GB; 6.0 TB free -> 4,600 GB)
ceiling  = min(fs_arm, knob_arm) = 2,031 GB   arm=knobs
pause    = 90 % of 1,934 GB = 1,741 GB
kill     = 2,031 GB, and only after 4 sustained samples past it
```

**The peak it holds to: 2,031 GB (pause at 1,741 GB), against a working peak of
~1,050 GB** (the measured tape tree peak `0.789 n` = 1,032 GB plus a ≤20 GB
position window), i.e. 40 % headroom below the pause and 2× below the kill. For
comparison, the incident-B run was killed at 2,325 GB by a ceiling of 2,162 GB; with
these numbers the same build is paused at 1,741 GB and stopped at 2,031 GB.

`--scratch-budget-gb` must satisfy the plan's RAM law (the sorter holds the band
window: `--memory-gb 96` allows ~100 GB of budget, `--memory-gb 205` allows 250 GB).
`xsa build --plan-only` prints the verdict on `XSA_PLAN_RAM`; the governor never
papers over an infeasible plan.

---

## 8. Residual risks

1. **The pile build currently running is the OLD binary** and its governor has the
   `kill(-1, SIGSTOP)` bug: on its next sample past its 250 GB pause threshold it
   will freeze every process of the user again. It must be relaunched with a fixed
   binary (this branch). Its state at the time of writing: paused (state `T`), its
   journal's last line 09:38:46 at `live_gb=251.115`.
2. **`term_merged` and `term_container` are extrapolations** of measured per-n
   rates (`0.364n` pfck2 is exact at the pile from incident B; `0.283n` chi and
   `0.468n` container are 1 GB-fixture ratios). They are 1.78x/1.5x allowances, and
   a breach is journaled (`SCRATCH_TERMS`/`SCRATCH_LEDGER_UNKNOWN`), but a corpus
   whose finish state is superlinear would move the knobs arm up, not down.
3. **The audit only compares on a quiescent tree.** `du` cannot read a tree that is
   being written, so the S1 comparisons are the moments at which two consecutive
   samples agreed (9 and 31 in the two runs, worst 0.13 %). The per-sample bracket
   is journaled for every sample for the record.
4. **The pause is anchored on the intent arm** (`min(ceiling, term_sum)`), so on a
   box with a huge `--scratch-budget-gb` the pause is late by construction — the
   filesystem arm is then the only thing between the run and the disc edge. That is
   the correct behaviour (the knobs declared the usage), but it means the knobs are
   still the first line of defence.
5. `XSA_SUBDIV_STRATEGY=local` (a sorter debug knob, not a product flag) made one
   512 MB fixture run crawl in the finish's LCP-repair pass until its timeout. It is
   unrelated to this patch (the same flags completed in 303 s twice), but a gate
   that relies on it should stay opt-in.
6. The stale `S1` clock: the audit's `du` runs inside the governor every quiescent
   sample; on a very large tree it adds a second full walk per such sample. It is
   opt-in (`XSA_SCRATCH_AUDIT=1`) for exactly that reason.
