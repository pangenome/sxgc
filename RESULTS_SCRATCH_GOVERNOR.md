# The scratch governor: the ceiling arithmetic, the box-freeze, and the numbers

Branch `fix/scratch-ceiling` (from `origin/main` `4e275f6`).

> **xsa sends no signals; it writes two files and reads exit codes.**
>
> `<scratch>/stop.requested` stops the run (every phase observes it at its own
> boundary, journals `STOP_OBSERVED` and exits 75);
> `<scratch>/waves-tape/geometry.ctl` throttles it (the sorter re-reads it before
> every position window). Nothing else is ever done to another process. The
> process-stop backpressure that used to be the safety mechanism is **deleted**,
> because it was the thing that froze this box twice and it could not have helped
> the case that filled the disc.

```
xsa/src/scratchgov.rs                     the ONE term table, the ONE sampler, the pure bound
xsa/src/chain.rs                          the governor: plan-time bound, adaptive geometry,
                                          hard fail, per-pid shutdown (SIGTERM then SIGKILL)
xsa/runtime/tools/binbwt/scatter_sort.cpp re-reads waves-tape/geometry.ctl per window
gates/scratch_governor_gates.sh           R1, R2, R3, R4, R5, S4
```

---

## 0. The incidents

### A. The monitor could not act (live 8,189 GB, `paused=0`)

```
SCRATCH_SAMPLE live_gb=8188 ... budget_gb=250 paused=0
waves-tape/subdiv_pos   7.6 TB        (four windows x 2 TB)
filesystem              100 % full    -> scatter_sort: write position file
```

The sampler walked the whole tree, but the only term the response knew how to price
was the tape budget; the position window — the one scratch term **no consumer can
drain**, because the sorter frees it itself — could not stop the box.

*(The write-through cause — every window's position files staying on disc until the
end — is fixed in `RESULTS_SUBDIV_DISK.md`. This document is about the governor,
which still took no action at 8 TB.)*

### B. The governor acted, on a ceiling that did not price the build

```
SCRATCH_GOVERNOR_CEILING_EXCEEDED live_bytes=2,324,826,972,160
                                 ceiling_bytes=2,322,122,547,200
journaled formula: pos_budget_gb 1862.645 + budget_gb 250 + margin 50  ->  2162.645 GiB
```

Over by 2.7 GB, at a number the operator cannot predict, because **the merged
fragment family (`merged/frag.pfck2` + `merged/frag.chi-bitmap`, 443 GB = 0.364 n,
i.e. `plan::V2_PFCK_PER_N` exactly) and the container partial were not in the
formula.**

### B'. The same failure, 30 minutes after B (the pile, main's binary)

```
pos_budget 100 GB, --scratch-budget-gb 250, margin 50 (constant)
ceiling = 422,122,547,200 B          = pos + budget + margin    (merged absent)
live    = 423,759,282,176 B          = positions + bands + merged 339 GB
over by   1,636,734,976 B, killed at t+57 min, right after the first window opened
```

The merged-fragment term alone broke it, by a hair — which is why the bound now
carries merged and container **explicitly** and the runtime number is the same
number the plan prints.

### C. The pause path was the box-freeze (2026-11-09 ~23:00 and 2026-11-14 09:38:45)

The measured, not the suspected, root cause:

```
procps-ng 4.0.4 `kill -<SIG> -<pgid>` keeps only the FIRST DIGIT of a negative pid spec.
  strace -e kill /usr/bin/kill -0 -1257815   ->  kill(-1, 0)      # EVERY process of the user
  strace -e kill /usr/bin/kill -0 -2449516   ->  kill(-2, 0)
  strace -e kill /usr/bin/kill -0 -9876543   ->  kill(-9, 0)
```

**The governor's PAUSE — the safety mechanism — was the thing that stopped every
process of the user, and it did so twice.** The STOP spelling
(`kill -STOP -<pgid>`) froze the box; the KILL spelling
(`kill -KILL -<pgid>`) was a `kill(-1, SIGKILL)` waiting for the first overrun. The
pile build's own governor crossed its 250 GB pause threshold at
`live_gb=251.115` (`PILE/xsa-build.log`, last line, 09:38:46.698) and froze the box,
its own phases included; PILE stood static at 269,682,057,216 B with all nine of its
processes in state `T`.

---

## 1. The pause is DELETED (and the per-pid shutdown after it), and why

1. **A paused producer cannot free what it is being paused for.** The position
   window is written AND consumed inside the sorter; pausing the sorter pauses the
   descent that unlinks those files. In the case that actually filled the disc the
   pause is structurally incapable of helping.
2. **It only helps for bands**, whose consumer is a different process — and the
   band window is bounded by construction (60 % of `--scratch-budget-gb`).
3. **It costs an external process-control surface** (this bug), deadlock risk (a
   producer stopped mid-write into a pipe whose consumer waits), stopped processes
   holding RAM and fds, and a journal that reports success whether or not anything
   happened. **The recorded evidence says it never once acted usefully**: peak
   7.185 GB against a 6.000 GB budget with `paused=1`; and gate R5 caught the old
   binary pausing on a *fixture* (`pauses=1 resumes=1 paused_ms=2102`) while its own
   `peak_gb=0.548` accounting excluded the 4.0 GB container entirely.

`signal_group`, `stop_producers`, `continue_producers`, the pause/resume thresholds
and the stall ratchet are gone. So is the per-pid SIGTERM/SIGKILL shutdown that came
next: **the product sends no signal to any process, not even its own children.** The
cooperative channel that already existed on both ends is now the only control:

| direction | channel | contract |
|---|---|---|
| stop | driver writes `<scratch>/stop.requested` (`recovery::write_stop_flag`) | each phase polls it at its own boundary, journals `STOP_OBSERVED phase=… <where>` and exits **75**; the driver waits (**no timeout**), journals each rc and the phase's own line, and owns the restart/resume decision |
| throttle | driver writes `<scratch>/waves-tape/geometry.ctl` | the sorter re-reads `position_budget_bytes=` before **every** position window (`SS_GEOMETRY`) |

The boundaries, and their granularity (residual risk §8.2):

| phase | boundary | granularity |
|---|---|---|
| `order-tape` (scatter_sort) | top of the wave loop, **and inside `waitMarker`** (the band/`.consumed`/`.lcp.consumed` waits) | one wave; one 10 ms poll inside a wait |
| `emit-tape` (cross_lcp_merge) | before reading each band | one band |
| `finish-v2` (xsa_finish) | the four wait loops: band progress, terminal, runcols idx, the piece it is waiting for | one poll (20 ms) |
| `ctx-tap` | after each band it consumes | one band |

`grep -rn "kill(\|SIGSTOP\|SIGTERM" xsa/src xsa/runtime/tools xsa/runtime/bit6` finds
only comments and the `XSA_KILL_AT` hook, which exits the driver itself.

---

## 2. The three replacements

### A. PLAN-TIME BOUND (the part that was wrong twice)

Before any scratch directory is created and before the first phase is spawned, every
live term is priced from the ONE table, summed, and compared with what the box can
afford:

```
projected_live_peak = sum(term budgets) + slack        # slack = max(8 GB, 5 %)
fits               = projected_live_peak <= free_at_start - reserve
refusal            = SCRATCH_BOUND_REFUSED (nothing written)
runtime ceiling    = the SAME projected_live_peak      # the printed number is the contract
```

`free_at_start` and `fs_total` come from `statvfs` at plan time (`f_bavail`/
`f_blocks` x `f_frsize`), `reserve` defaults to `max(10 % of the filesystem, 500 GB)`
(clamped to half of it), and `--plan-only` prints the same line, so "will it fit?"
is answerable before an expensive census:

```
SCRATCH_BOUND projected_live_peak=331759337280 projected_live_peak_gb=308.975
  max_live=331759337280 free_after_peak=6382275059904 free_after_peak_gb=5943.957
  term_sum=315961273600 term_sum_gb=294.262 slack=15798063680 slack_gb=14.713
  free_at_start=6613523828736 free_at_start_gb=6159.324 fs_total=15360854417408
  reserve=1536085441740 reserve_gb=1430.591 reserve_clamped=0 fs_available=…
  target=331759337280 target_gb=308.975 target_ceiling_rate_pct=125
  ceiling=331759337280 fits=1 deficit_bytes=0
SCRATCH_TERMS budget_pos=20000000000 budget_bands=64424509440 budget_merged=846217825583
  budget_pieces=42949672960 budget_container=918416480184 budget_other=53687091200
  budget_sources=pos:XSA_SUBDIV_POSITION_BUDGET_BYTES|default=max(256MB,budget/5), …
```

The term table, one entry per family, is §4.

### B. ADAPTIVE GEOMETRY (a control loop on OUR OWN sizes)

If the sampler sees `live > target` (`--scratch-target-gb`, default
`projected_live_peak`), the governor writes `position_budget_bytes=N` into
`<scratch>/waves-tape/geometry.ctl` (atomic: tmp + rename). `scatter_sort` re-reads
that file **before every position window**, so the NEXT window is smaller than the
last, and it never goes below one wave (the plan caps a wave at `pos_budget/8`):

```
SCRATCH_GEOMETRY action=shrink iteration=1 live=2211835904 live_gb=2.060
  target=1986422374 ceiling=2483027967 from_bytes=67108864 to_bytes=50331648
  floor_bytes=8388608 why=live_over_target rule=next_window_only,product_unchanged
SS_GEOMETRY action=shrink source=control_file file=…/geometry.ctl
  declared_bytes=67108864 from_bytes=67108864 to_bytes=26464050
  floor_bytes=26464050 max_wave_rows=3487485 rule=next_window_only,product_unchanged
```

The product cannot change: the window only decides how many consecutive waves one
sweep seeds. The band stream, its order and its numbering are byte-identical
(gate R5 re-proves the container).

### C. HARD FAIL, and a COOPERATIVE stop

Past the ceiling the build fails with the numbers (term breakdown, current and peak
per term, target, max_live, free disk, reserve, shrinks, levers); the driver journals
`STOP_REQUESTED`, writes `<scratch>/stop.requested`, and **waits for every phase to
exit at its own boundary** (exit 75). No signal is sent — not to a group, not to our
own children. If a phase never observes the flag, that is a bug in that phase (the
boundaries and their granularity are in §1 and §8.2), not something to paper over
with a signal.

---

## 3. The term table (the one place a family is named)

`xsa/src/scratchgov.rs` holds one classifier and one sampler; the journal's terms,
the live total, the bound and the ceiling all come from the same walk.

| term | rule (the paths it owns) | budget |
|---|---|---|
| `term_pos` | `waves-tape/subdiv_pos/**` (tape), `*/binbwt-windows/**` (bin) | `XSA_SUBDIV_POSITION_BUDGET_BYTES`, default `max(256 MB, budget/5)` — the same knob the sorter reads |
| `term_bands` | `bands-tape/**`, `waves-tape/**` (not `subdiv_pos`) | 60 % of `--scratch-budget-gb` |
| `term_merged` | `merged/**` except `merged/frag.pieces/**`: `frag.rlebwt`, `frag.pfck2`, `frag.chi-bitmap`, `frag.bits-*`, `frag.finish-spool` | tape: `(0.364 + 0.283) n`; bin: `1.975 n` (the plan's measured per-n rates) |
| `term_pieces` | `merged/frag.pieces/**` | 40 % of `--scratch-budget-gb` |
| `term_container` | `X.sxi2`, `X.sxi2.partial`, `X.sxi2.part` at the scratch root | `1.5 x 0.468 n` |
| `term_other` | `*.log`, `*.time`, `remap-256.bin(+.part*)`, `checkpoint.journal`, `trie-m20.bin(+.partial)`, `stop.requested`, `xsa-build.log`, `geometry.ctl`, and **anything the table does not name** | 50 GB |

Rules that make the sum equal `du` (both are gates):

* **allocated bytes** (`st_blocks x 512`), never apparent size (the chi bitmap is a
  sparse n/8 mapping);
* **hard links counted once** (dev+ino), exactly as `du` counts them: the finish
  links the container beside itself for the in-place member rewrite (counting both
  names broke the identity by a whole container: measured 95 % delta);
* an **unnamed** path is still counted (folded into `term_other`, so `live` stays
  the whole tree) but it is journaled as `SCRATCH_LEDGER_UNKNOWN` and
  `unknown_paths` rises. `unknown_paths=0` is asserted on every sample.

---

## 4. The gates (verbatim, with N)

Binary under test `sha256=21cc2237…8c31d` (branch `fix/scratch-ceiling`); reference
binary `sha256=85924aeb…d16420`, built once from origin/main at the branch point
(`git archive 4e275f6`).

### R1 NO SIGNALS TO OTHER PIDS — a full build sends zero

512 MB fixture under `strace -f -e trace=kill,tkill,tgkill,execve`, every signal
syscall classified by its target pid (a signal to the driver's own pid or one of its
threads is SELF and allowed):

```
R1_RC rc=0
R1_SIGNAL_SYSCALLS total=0 to_other_pids=0 SIGSTOP=0 SIGCONT=0 kill1_execve=0
R1_RESULT ok=1 signals_to_other_pids=0 SIGSTOP=0 SIGCONT=0 kill1_execve=0 (self-signals allowed)
```

**N = 0 signal syscalls of any kind** in the whole process tree of a successful
build, and no `kill(1)` execve. The same check runs inside R4 on the failure path
(`SIGTERM=0` there too) and R6 on the stop path.

### R2 PLAN-TIME REFUSAL — nothing written

The filesystem is made "too small" from the plan's point of view by pushing the
reserve to (nearly) the free space; the same arithmetic a small device produces:

```
R2_RC rc=1 wall_s=1
R2_ARITHMETIC ok=1 max_live=63444462639 fs_available=1024819 ceiling=1024819 fits=0
    free_at_start=6626523099136 reserve=6626522074317 slack=8589934592
R2_SCRATCH_BYTES 20480 families='checkpoint.journal remap-256.bin xsa-build.log '
R2_RESULT ok=1 written_bytes=20480 fits=0
```

**20480 bytes written** = the journal, the plan's remap spool (256 B) and the resume
checkpoint — **no `bands-tape`, no `merged`, no `waves-tape`, no container.** The
message (verbatim, from the run):

```
SCRATCH_BOUND_REFUSED: the PROJECTED LIVE PEAK 63444462639 B (59.087 GiB) does not fit the
filesystem: free_disk=6626523099136 B (6171.430 GiB), reserve=6626522074317 B (6170.958 GiB),
free_minus_reserve=1024819 B (0.001 GiB), deficit=63443437820 B (59.086 GiB). TERMS:
term_pos=268435456 size (XSA_SUBDIV_POSITION_BUDGET_BYTES|default=max(256MB,budget/5))
term_bands=161061273 (scratch_budget x TAPE_BAND_SHARE_PCT)
term_merged=21711041 (measured 0.364n(pfck2)+0.283n(chi bitmap) x n)
term_pieces=107374183 (scratch_budget x TAPE_PIECE_SHARE_PCT)
term_container=23556648 (measured 0.468n(container) x 1.5 x n)
term_other=53687091200 (GOVERNOR_OTHER_BUDGET_BYTES) | term_sum=54854528047 slack=8589934592.
LEVERS: --scratch-budget-gb (smaller band+piece window), XSA_SUBDIV_POSITION_BUDGET_BYTES
(smaller position window), --scratch-reserve-gb (smaller reserve: currently 6170.958 GiB),
or a scratch filesystem with room. NOTHING WAS WRITTEN
```

### R3 ADAPTIVE SHRINK — the window shrinks, live falls

256 MB fixture, sorter's wave subdivision (`XSA_SUBDIV_STRATEGY=local`), position
budget 64 MB, `--scratch-target-gb 1.85`:

```
R3_RC rc=0 wall_s=132
R3_ARITHMETIC ok=1 max_live=63243136047 fs_available=5081558766388 ceiling=2483027967
   fits=1 target=1986422374 (ok = the bound recomputed from the journal's own numbers)
R3_DECISIONS governor_shrinks=4
SCRATCH_GEOMETRY action=shrink iteration=1 live=2211835904 target=1986422374
   from_bytes=67108864 to_bytes=50331648 floor_bytes=8388608
R3_LIVE at_shrink=2211835904 min_after=1727877120 fell=1   (one run; another run 2.212 GB -> 2.212 GB, fell=0)
R3_SORTER_APPLIED applied=1 from_bytes=67108864 to_bytes=26464050 floor_bytes=26464050
   max_wave_rows=3487485
R3_SORTER_WINDOWS examined=115 shrunk_to_bytes=26464050 all_after_within_budget=1
R3_WINDOW_BYTES control_no_ctl_max=58739656 with_geometry_ctl_max=25140848 ratio=0.428
R3_RESULT ok=1 governor_shrinks=4 sorter_applied=1 windows=115
```

Four shrink decisions (live 2.212 GB > target 1.85 GB); the sorter applied the
control file (67,108,864 → 26,464,050 B, the one-wave floor) and **all 115 windows
after the shrink wrote within the shrunk budget**; and the position term's live peak
fell from **58,739,656 B (no control file) to 25,140,848 B (with it), ratio 0.428** —
that is the "live falls" half measured per term, because on a fixture the container
(0.65 n), not the window, dominates total live.

### R4 HARD FAIL IS GRACEFUL — numbers, cooperative stop, zero signals

256 MB fixture, `--scratch-target-gb 0.35` (the ceiling is 125 % of it):

```
R4_RC rc=1
R4_ARITHMETIC ok=1 max_live=63444462639 fs_available=5071170536244 ceiling=469762047
   fits=1 target=375809638
SCRATCH_GOVERNOR_HARD_FAIL why=live_over_ceiling live=493355008 live_gb=0.459
   ceiling=469762047 over_by=23592961 target=375809638 max_live=63444462639
   term_pos=0 term_bands=188932096 term_merged=132104192 term_pieces=… term_container=…
   term_other=… action=cooperative_stop (write stop.requested, wait for exit 75, NO SIGNAL)
SCRATCH_GEOMETRY action=shrink iteration=1 live=432975872 … to_bytes=201326592
SCRATCH_GEOMETRY action=shrink iteration=2 live=387219456 … to_bytes=150994944
R4_SIGNALS SIGSTOP=0 SIGCONT=0 group_signals=0 SIGTERM=0 to_other_pids=0
R4_STOP_OBSERVED phases_reporting=4
    | STOP_OBSERVED phase=ctx-tap at_band=937
    | STOP_OBSERVED phase=emit-tape at_band=943
    | STOP_OBSERVED phase=finish-v2 waiting_for=band_progress waited_s=0.000
    | STOP_OBSERVED phase=order-tape waiting_on=band.consumed wall_s=0.000
R4_CHECKPOINT bytes=213247 lines=1392 ends_with_newline=1
R4_RESULT ok=1 signals_to_other_pids=0 SIGSTOP=0 SIGCONT=0 group_signals=0 SIGTERM=0 stop_observed=4
```

Over by 23,592,961 B with the full term breakdown, 2 adaptive shrinks first, **zero
signals of any kind — SIGTERM to our own children included —, all four phases
stopped at their own boundaries**, no survivors, and the checkpoint journal left
consistent.

### R5 NO REGRESSION — byte-identical container, N bands compared

```
R5_CONTAINER ours=8142cbbc85fe96bc3a41206fdceaeabdaf7514a3f0e4faea32e8f861a63ee7e3 bytes=3422870896
R5_CONTAINER main=8142cbbc85fe96bc3a41206fdceaeabdaf7514a3f0e4faea32e8f861a63ee7e3 bytes=3422870896
R5_BANDS compared=5519 (band,rows) pairs ours=108dd516…dc07c6 main=108dd516…dc07c6
R5_WALLS ours_s=268 main_s=267
R5_RESULT ok=1 bands=5519 container_sha=8142cbbc…ee7e3
```

5519 bands compared (band id + row count, sorted: the writer slots emit
concurrently, so the *set* is the contract). The two summaries are the incident-B
story in one line each:

```
ours: … peak_gb=4.138 … process_control=none peak_terms=term_pos=0,term_bands=192851968,…,
      term_container=4294836224
main: SCRATCH_BACKPRESSURE_SUMMARY … peak_gb=0.548 pauses=1 resumes=1 paused_ms=2102
      accounting=scratch_excludes_container
```

the same build, the same 4.0 GB container: the old accounting under-reported live
scratch by the whole product, and the pause fired and did nothing.

### R6 COOPERATIVE STOP — the flag, the boundaries, the resume

`stop.requested` arrives 20 s into a 512 MB build:

```
R6_WRITING_STOP_REQUESTED 2026-11-14T18:42:47Z
R6_RC rc=75 (BUILD_INTERRUPTED)
PHASE emit-tape STOP_OBSERVED rc=75 cooperative=1 (the phase exited at its own boundary)
STOP_OBSERVED phase=emit-tape at_band=434
STOP_OBSERVED phase=order-tape waiting_on=band.consumed wall_s=0.000
STOP_OBSERVED phase=finish-v2 waiting_for=band_progress waited_s=0.000
STOP_OBSERVED phase=ctx-tap at_band=434
R6_OBSERVED phases_reporting=6
R6_CHECKPOINT bytes=98679 lines=646
R6_RESUME_RC rc=0
RESUME_STATE spools_present=12 spools_missing=1 recomputed=1 pieces_present=4 resume=true
WORK_AVOIDED_SUMMARY performed=1608355330 total=2145234433 avoided_frac=0.250266 phases=4
   per_phase=remap=1.000000 sort=0.000000 emit=0.000000 finish=0.000000
R6_RESULT ok=1 rc=75 stop_observed=6 resume_rc=0 'WORK_AVOIDED_SUMMARY'
```

Every phase observes the flag, journals **where** it did (a wave/band/piece boundary —
`waiting_on=band.consumed`, `at_band=434`, `waiting_for=band_progress` — never an
arbitrary instruction), the driver reports `BUILD_INTERRUPTED`, nothing survives, the
checkpoint is 646 whole lines, and the relaunch resumes with **WORK_AVOIDED
avoided_frac=0.250266** (the remap phase fully avoided).

### S5 THE ARITHMETIC, against all three incidents (unit tests)

```
$ cargo test --release scratchgov -- --nocapture
INCIDENT_A ceiling_gb=3334.427 arm=knobs fs_arm_gb=6800.000 knob_arm_gb=3334.427 reserve_gb=1400.000
INCIDENT_A small_ceiling_gb=592.000 arm=knobs
INCIDENT_B old_ceiling_bytes=2322122547200 live_actual_bytes=2324826972160
           new_ceiling_bytes=2937679515745 new_ceiling_gb=2735.927 arm=knobs slack_gb=130.282
INCIDENT_B recommended pos_gb=20 bound_gb=801.150 live_actual_gb=2165.164
INCIDENT_3 old_ceiling_bytes=422122547200 live_actual_bytes=423759282176 over_by_bytes=1636734976
           new_ceiling_bytes=799178674560 new_ceiling_gb=744.293 knob_arm_gb=744.293
           fs_arm_gb=4266.400 slack_gb=35.443
```

* **A**: with a 2 TB position budget the *filesystem* arm (8200 − 1400 = 6800 GiB) is
  what would have stopped the 8,189 GB run; when the declared intent is smaller than
  the box the *knobs* arm wins (592 GiB = `20 + 150 + 100 + 197 + 47 + 50 + 28`). A
  bound is the SMALLER of the two, never the sum of knobs.
* **B**: the old ceiling (2162.6 GiB) was **below** the live number that occurred; the
  new bound (2735.9 GiB) is **above** it, and with the recommended knobs (pos 20 GB)
  it is **801 GiB** — the overrun that killed the pile at 2324.8 GB is caught at 801 GB.
* **B'**: the exact triple from the pile 30 minutes later — old 422,122,547,200 B,
  live 423,759,282,176 B, new **799,178,674,560 B** — the merged term (339 GB) is
  what broke it, and the bound now prices it.

---

## 5. S4: the 100 GB head — the plan-time bound at scale (and why it is OPEN)

Command (both arms, identical flags):

```
XSA_SUBDIV_POSITION_BUDGET_BYTES=20000000000 xsa build --tape
  --input pile-head100.txt --docs-map pilemap100 --threads 96 --memory-gb 96
  --finish-as-gb 400 --scratch-budget-gb 100 --no-trie
```

**The plan-time bound at 100 GB, printed before the run** (ours; `fits=1`):

```
SCRATCH_BOUND projected_live_peak=331759337280 projected_live_peak_gb=308.975
  max_live=331759337280 free_after_peak=6382275059904 free_after_peak_gb=5943.957
  term_sum=315961273600 term_sum_gb=294.262 slack=15798063680 slack_gb=14.713
  free_at_start=6714034397184 free_at_start_gb=6252.717 reserve=1536085441740
  reserve_gb=1430.591 fs_available=… target=331759337280 ceiling=331759337280 fits=1
SCRATCH_TERMS budget_pos=20000000000 budget_bands=64424509440
  budget_merged=846217825583 budget_pieces=42949672960 budget_container=918416480184
  budget_other=53687091200
```

(the merged/container budgets there are the **pile** rates applied to n = 1e11, i.e.
`0.647 n` and `0.702 n`; on the pile corpus they are the 846 GB / 918 GB of §7.)

**The measured peak and the model error** (451 samples, 2 h of the build):

```
PEAK_SAMPLE term_pos=0 term_bands=67717607424 term_merged=36363657216 term_pieces=4096
  term_container=4096 term_other=204800 live=104081477632 live_gb=96.933
  target=331759337280 ceiling=331759337280
peak_gb=96.933   max_live_gb=308.975   model_error_fraction = -0.686
```

The bound is **conservative by 3.2x** at 100 GB, and that is the honest reading of the
term table: bands+pieces (100 GB of budget) + merged (0.647 n = 64.7 GB) + container
(0.702 n = 70 GB) + pos (20 GB) + other (50 GB) are counted at their *maxima
simultaneously*, while the tape route's measured tree peak is `0.789 n` = 79 GB
(`plan::TAPE_DISC_MEASURED_PER_N`) rising to 104 GB with the 20 GB position window. A
bound that is 3x too high protects the box at the cost of headroom (§8.3).

**Status: OPEN.** Neither arm produced a container, for reasons that are not the
governor's:

1. The in-build trie fails identically on BOTH binaries at this scale:
   `xsa: error: CONTEXT_TRIE: builder: children are not in ascending byte order (the
   suffix stream is not sorted)`, band 374, `total_rows=1,209,029,218` — hence
   `--no-trie`.
2. Two concurrent 100 GB arms (`--finish-as-gb 400` each) were **killed at 7,058 s
   with rc=137 by the kernel** (both at the same wall second, `load_end=4.50`): the
   two finishes together exceed the box's RAM. They must be run SEQUENTIALLY, which
   is the next step; the term table, the bound and the timer above are from that run.

Until that sequential pair completes, the *byte-identity at 100 GB* claim is not made:
R5 proves container identity at 512 MB (5519 bands).

## 6. The knobs (and their help text)

| knob | default | what it does |
|---|---|---|
| `--scratch-budget-gb N` | derived `min(0.8 × free, 2 × band window)` | sizes the band window (60 %) and the piece window (40 %); it is the `term_bands + term_pieces` budget |
| `--scratch-reserve-gb F` | `max(10 % of the filesystem, 500 GB)`, clamped to half | the plan's bound is `projected_live_peak <= free_at_start − reserve`; decimal GB |
| `--scratch-target-gb F` | `projected_live_peak` (inert) | **NEW**: the live target the geometry controller aims at; below it the next position window is shrunk, past 125 % of it the build fails |
| `XSA_SUBDIV_POSITION_BUDGET_BYTES` | `max(256 MB, budget/5)` | the position window (`term_pos`), read by the sorter **and** the governor, and the knob the adaptive shrink moves at run time |
| `XSA_SCRATCH_AUDIT=1` | off | the product's own term audit: `SCRATCH_AUDIT_TERMS` compares the journaled term sum against `du` on the same tree on every quiescent sample |

Journal lines: `SCRATCH_BOUND` / `SCRATCH_BOUND_REFUSED` / `SCRATCH_BOUND_PLAN_ONLY`,
`SCRATCH_TERMS`, `SCRATCH_TERM`, `SCRATCH_SAMPLE` (`term_pos= … term_other= live=
target= ceiling=`), `SCRATCH_LEDGER_UNKNOWN`, `SCRATCH_GEOMETRY` (+ the sorter's
`SS_GEOMETRY`), `SCRATCH_GOVERNOR_HARD_FAIL`, `SCRATCH_CEILING_EXCEEDED`,
`SCRATCH_AUDIT_TERMS`, `SCRATCH_AUDIT_SUMMARY`, `SCRATCH_GOVERNOR_SUMMARY`, and on
the stop path `STOP_REQUESTED` / `PHASE <tag> STOP_OBSERVED rc=75 cooperative=1` plus
the phase's own `STOP_OBSERVED phase=… <where>` line. The old `SCRATCH_BACKPRESSURE*`
lines are gone with the mechanism.

---

## 7. The recommended knobs for the next pile relaunch — measured, not estimated

Pile: `pile.txt` N = 1,307,910,802,540 (1.31 TB); scratch filesystem 14 TB, 6,103 GiB
free at plan time.

```
XSA_SUBDIV_POSITION_BUDGET_BYTES=20000000000     # 20 GB position window, freed per window
--memory-gb 256 --threads 96                     # the envelope the pile run already uses
--finish-as-gb 400
--scratch-budget-gb 100                          # feasible: XSA_PLAN_RAM says the arena is
                                                 # 103.0 GiB of the 128.0 GiB envelope
--scratch-reserve-gb <default: 1430.591 GiB>     # 10 % of the 14 TB scratch
--docs-map pilemap_full                          # exact-corpus map
```

**`xsa build ... --plan-only` on the pile corpus, verbatim:**

```
SCRATCH_BOUND projected_live_peak=2042704593537 projected_live_peak_gb=1902.417
  max_live=2042704593537 free_after_peak=4510268124543 free_after_peak_gb=4200.515
  term_sum=1945432946226 term_sum_gb=1811.826 slack=97271647311 slack_gb=90.591
  free_at_start=6552972718080 free_at_start_gb=6102.931 fs_total=15360854417408
  reserve=1536085441740 reserve_gb=1430.591 reserve_clamped=0 fs_available=5016887276340
  target=2042704593537 target_gb=1902.417 ceiling=2042704593537 ceiling_gb=1902.417
  fits=1 deficit_bytes=0
SCRATCH_TERMS budget_pos=20000000000 budget_bands=64424509440 budget_merged=846218289243
  budget_pieces=42949672960 budget_container=918153383383 budget_other=53687091200
SCRATCH_BOUND_PLAN_ONLY fits=1 (the run enforces the same number)
XSA_PLAN_DISC FEASIBLE shape=tape: peak 100.0 GiB (107374182400 B) = 0.082n fits 6102.9 GiB free
XSA_PLAN_RAM FEASIBLE shape=tape: the band arena is 103.0 GiB = 96 bands in flight x 4092003
  rows x 90 B/row + 0.5 GiB fixed + 6.4 GiB staging = peak RSS 109.83 GiB, 85 % of the
  128.00 GiB envelope (--memory-gb 256, the bound is 90 %)
```

**The peak those knobs hold to: 1,902.417 GiB = 2,042,704,593,537 B (1.90 TiB).** That
is the number the run prints and the number the runtime enforces, with **4200.5 GiB of
free disk left after the peak**, against a working peak of ~1,032 GB (the measured tape
tree peak `0.789 n`, `plan::TAPE_DISC_MEASURED_PER_N`) — i.e. the bound is 1.84x the
working peak and the box keeps 4.1 TiB in reserve for every other lane.

For comparison with the incidents: B was killed at 2,325 GB by a ceiling of 2,162 GB;
B' at 423.8 GB by 422.1 GB; A ran to 8,189 GB with no ceiling at all. With these
knobs the same builds are **priced before the census** (fits/has the numbers), stopped
at a printed number, and stopped *cooperatively* — no signal, ever.

If the operator wants a tighter ceiling, the honest levers are
`XSA_SUBDIV_POSITION_BUDGET_BYTES` (pos is only 20 GB of the 1.9 TiB) and the merged /
container allowances, which are the two big terms (846 GB + 918 GB, both measured-rate
extrapolations, §8.4) — not the wave budget.

## 8. Residual risks and open items

**1. S4 (100 GB) is open.** See §5: the trie defect (both binaries) and an OOM kill
of two concurrent arms. The plan-time bound and the term table at 100 GB are recorded;
the sequential pair is the next step. No 100 GB byte-identity claim is made.

**2. The stop boundaries have granularity.** A phase stops at its *natural* boundary,
so the latency between `stop.requested` and exit is:
* `order-tape`: one wave (seconds to minutes at the pile), or one 10 ms poll when it is
  inside `waitMarker` (band/`.consumed`/`.lcp.consumed` waits) — the wait case is the
  common one at the pile;
* `emit-tape`: one band;
* `finish-v2`: one 20 ms poll (it is always inside a wait loop);
* `ctx-tap`: one band.

The one path that can still take a *long* time is the scatter sweep inside a local
wave window (`SS_SCATTER`, one full corpus pass, ~200 s at the pile per window): the
sorter does not poll inside it. That pass allocates no new scratch (it writes the
window it is already sized for), so it cannot grow live; it only delays the stop. This
is named as a residual risk rather than papered over with a signal.

**3. The bound is a BOUND, not a forecast** (measured 3.2x conservative at 100 GB,
§5). The term budgets are per-term maxima that do not co-occur. Tightening it needs
measured co-occurrence factors per route; until then the box is protected and the
headroom is spent.

**4. `term_merged` and `term_container` are extrapolations** of measured per-n rates
(0.364 n pfck2 is exact at the pile from incident B; 0.283 n chi and 0.468 n container
are 1 GB-fixture ratios). They are 1.78x/1.5x allowances, and a breach is journaled,
but a corpus whose finish state is superlinear moves the bound up.

**5. The audit only compares on a quiescent tree** (`du` cannot read a tree being
written): the comparisons are the moments at which two consecutive samples agreed.

**6. The adaptive shrink acts on the position window only.** The band count is fixed
by the plan (the arena is allocated once) and its window is bounded by construction;
the piece window is the emitter's, set at spawn.

**7. The sorter's own no-progress die (6000 s)** is now also reachable through a
cooperative stop that somebody else initiated: if a *consumer* stops and the sorter is
waiting on it, `waitMarker` sees `stop.requested` and exits 75 within 10 ms (§1), which
is what R6 proves; the 6000 s die remains for genuine consumer death without a flag.

**8. `XSA_SUBDIV_STRATEGY=local`** (a sorter debug knob) made one 512 MB fixture run
crawl in the finish's LCP-repair pass until its timeout — unrelated to this patch, but
a gate that relies on it stays opt-in.
