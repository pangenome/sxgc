# The position window: the incident, the root cause, the fix, the numbers

Branch `fix/subdiv-window-disk` (from `origin/main` 6da7b9d). Fixes the disk bug that
killed the pile run; makes the peak of the position column an ASSERTION instead of
a hope.

```
xsa/runtime/tools/binbwt/scatter_sort.cpp   the interleave, the budget guard, the statvfs guard,
                                            SS_SUBDIV_POSPEAK / POSSPACE / SWEEPS / WAVEGOAL
xsa/src/chain.rs                            the governor prices the position window
                                            (SCRATCH_GOVERNOR, hard ceiling, journaled ratchet)
gates/subdiv_disk_gates.sh                  D1 (parity), D2/P1-P2 (peak + sweeps), D3 (identity),
                                            D4 (tight budget), D5/P4 (refusal), P6 (clean exit)
```

## 1. The incident (measured, 2026-11)

The 1.31 TB pile run, `XSA_SUBDIV_POSITION_BUDGET_BYTES=2000000000000`, died after
2 h with `scatter_sort: write position file`:

```
SCRATCH_SAMPLE live_gb=8188 ... budget_gb=250 paused=0
waves-tape/subdiv_pos   7.6 TB        (four windows x 2 TB)
filesystem              100 % full
```

Live scratch reached 8.2 TB against a 250 GB budget, and `paused=0`: the
backpressure controller never held the producer, because the position column is the
one scratch term **no consumer can drain** — the sorter frees it itself — so the
stall path ratcheted its pause threshold upward and let the box fill.

## 2. Root cause (in the code, not in the doc)

`scatter_window()` wrote a window's position files, the window loop moved to the
NEXT window, and the DESCENT that consumes those files lived in a separate per-wave
loop. Every window's position files therefore stayed on disc until the very end:

```
peak = POSITION BYTES FOR EVERY LOCAL ROW = local_rows x 8 B      (~10.5 TB at the pile)
```

That is a **corpus** number, not a budget number: no value of
`--subdiv-position-budget-bytes` could lower it, and `RESULTS_SUBDIVISION.md`'s
claim that "the position files of one window are unlinked as soon as the window's
waves are descended" was **false in the shipped code** — the unlink existed, but it
ran only after every window had already been written. Corrected in place; see §7.

## 3. The fix

### 3.1 The window is sown lazily and freed at its own end (`scatter_sort.cpp`)

The window loop is gone. The wave loop opens a window the moment it reaches a local
wave with no window on disc, and frees it after the window's last wave:

| what | where |
|---|---|
| `posOpenWindow(w)` builds the window `[w, w1)` from `rowsEst` and calls the scatter | `scatter_sort.cpp:2069` |
| the emit loop calls it before the wave's descent | `scatter_sort.cpp:2419` |
| the window is unlinked + journaled after its last wave, with a hard assertion that the directory drained to 0 | `scatter_sort.cpp:3303` |
| `SS_SUBDIV_POSPEAK window= bytes_written= bytes_at_descent= bytes_after_unlink= peak_total= peak_window=` | `scatter_sort.cpp:3314` |

Waves are in key order and a window is a **consecutive range** of them, so the band
order, the band numbering (`globalBand`), `prev_wave_last` (the wave-seam LCP) and
the producer-state publications are byte-for-byte what they were: only the moment
the positions are written moved. That is what makes the band stream identical
(§4, P5) while the peak becomes one window.

### 3.2 The budget is a hard guard, not a planning target

* `waveGoal` is capped at `pos_budget x 95 % / 8` (`scatter_sort.cpp:1362`), so **no
  local wave can need more positions than one window may hold**. A class bigger than
  that is split into key-ordered children by the plan — the only order-safe shrink.
  Journaled as `SS_SUBDIV_WAVEGOAL ... binding=position-budget|band-plan`.
* windows are planned to **95 %** of the budget because `rowsEst` is a sample
  estimate (G8 measured ≤ 0.74 % error at 512 MB);
* the writer counts the bytes it writes and **DIES** past `posBudget`
  (`pos_budget_die`, `scatter_sort.cpp:1893`), naming the window, the actual bytes,
  the budget and the lever. A budget that is not enforced is not a budget.

### 3.3 The filesystem is guarded before every window

`statvfs` on the scratch filesystem, every window, before a byte is written
(`scatter_sort.cpp:1926`):

```
SS_SUBDIV_POSSPACE window=[wa,wb) free_bytes= needed_bytes= margin_bytes= budget_bytes= ok=1
```

`free < needed + margin` is a `die()` naming free / needed / margin / the knob /
the scratch dir. The margin is a knob — `XSA_SUBDIV_SCRATCH_MARGIN_BYTES`, default
50 GB (`SS_SCRATCH_MARGIN_BYTES`) — because on a real build the **container grows
while the windows are written**; the pile relaunch sets **200 GB**. A full disc
corrupts every other lane on this shared box; refusing is the only acceptable
failure mode.

### 3.4 The governor now prices the position window (`xsa/src/chain.rs`)

`SCRATCH_SAMPLE live_gb` already counted `subdiv_pos` (it walks the whole scratch
tree), but the pause logic could not act on it. Now:

* the governor reads the **same knob the sorter reads**
  (`XSA_SUBDIV_POSITION_BUDGET_BYTES`, default a fifth of the scratch budget) and
  journals its model once at build start (`SCRATCH_GOVERNOR budget_gb= pos_budget_gb=
  ceiling_gb= margin_gb= pause_at_gb= ... rule=pause_on_the_budget,
  ratchet_never_past_the_ceiling,fail_past_the_ceiling`, `chain.rs:1027`);
* every `SCRATCH_SAMPLE` line now carries `pos_budget_gb=` and `ceiling_gb=`;
* the stall ratchet may raise the pause threshold — a pause that can never drain
  must not deadlock the build — but never past
  `budget + pos_budget + margin`, and every ratchet is journaled with
  `why=residual_is_the_producers_position_window_not_a_drain` (`chain.rs` in
  `observe`, the `SCRATCH_GOVERNOR ratchet` line);
* **past the ceiling the governor KILLS the producers and fails the build with the
  numbers** (`SCRATCH_GOVERNOR_CEILING_EXCEEDED`, `chain.rs:1070`). At the pile that
  ceiling is 250 GB + 2 TB + 50 GB = 2.3 TB: the incident would have stopped at
  2.3 TB with a message instead of filling the disc at 8.2 TB.

## 4. The gates

Pre-fix binary: `origin/main` `665426745f96aa69...` (the binary the pile run used —
the same sha `RESULTS_SUBDIVISION.md` certified). Every product run is wrapped in
`setsid timeout --kill-after=10 N` and the harness asserts no survivor. Every gate
prints N of the things it examined.

### D1 fixture parity — 7 fixtures x 2 arms, byte-identical

```
SUBDIV_DISK_D1 fixture=docsep   arm=auto  rows=6291457 sha=539522013cbe992a... windows=0
SUBDIV_DISK_D1 fixture=docsep   arm=local rows=6291457 sha=539522013cbe992a... windows=1
SUBDIV_DISK_D1 fixture=mixed    arm=auto  rows=6291457 sha=2b1c5fa341a19945... windows=7
SUBDIV_DISK_D1 fixture=mixed    arm=local rows=6291457 sha=2b1c5fa341a19945... windows=8
... (eofrun, internsep, period2, r0, single)
SUBDIV_DISK_D1_PASS fixtures=7 arms=2 rows=44040199 (band stream byte-identical, sa_check PASS, subdiv_pos empty)
SUBDIV_DISK_NO_SURVIVORS checked=28 runs
```

The `auto` arm at 6 MB and 10 GB is **all-global** (page_fraction ≈ 1 for every
class), i.e. it never creates a window: a peak gate over it alone would be a vacuous
pass. The `local` arm (`--subdiv-strategy=local`) is therefore mandatory in every
window gate, and it is the arm that exercises the interleave (1–8 windows per
fixture, drained to 0 files at exit).

An ASAN run of the `mixed` fixture caught a real bug in the first cut of the fix —
`scatter_window` was block-scoped while `posOpenWindow` captured it by reference
(`stack-use-after-scope` → `std::bad_alloc` at wave 23). Fixed before the commit;
that is why the fixture's `bad_alloc` is recorded here rather than discovered later.

### D5 / P4 refusal — a near-full filesystem

```
SUBDIV_DISK_D5 scratch=/run/user/1001/sxgc-d5 free_bytes=43771224064       (tmpfs, 60 % full)
SUBDIV_DISK_D5 rc=2 files_written_before_refusal=0
subdiv position window does not fit the scratch filesystem: free_bytes=43771224064
  needed_bytes=79971602984 (the window's rowsEst x 8) margin_bytes=53687091200
  window=[0,31) scratch_dir=/run/user/1001/sxgc-d5/subdiv_pos
  . THE KNOBS: free space, or lower --subdiv-position-budget-bytes ... Refusing to fill the disc
SUBDIV_DISK_D5_PASS (refused with free/needed/margin instead of filling the disc)
```

`subdiv_pos` held **zero** files: the guard is reached before the first write.

### D2/P1–P3 100 GB head (`pile-head100.txt`, N = 1e11 + 1), pos_budget 20 GB

Two arms of the same binary: the product's own strategy (auto) and the forced-local
arm (the only one whose windows appear early — at 100 GB the plan's own choice is
`local=6 global=53`, and those 6 local classes are the deep sparse ones, i.e. late in
the run). Walls below were taken with TWO other 100 GB sorter arms running on the
same box (96 + 96 threads), so they are loaded-box numbers, not solo numbers.

```
# the product's own strategy (auto): the plan, and the sweep arithmetic it implies
SS_PLAN waves=59 local=6 global=53 cache_depth=128 ... corpus_reads=0
SS_SUBDIV_WAVEGOAL wave_goal=2375000000 band_goal=2926829260 position_budget_bytes=20000000000
                   position_goal_rows=2375000000 binding=position-budget
SS_SUBDIV_POSBUDGET position_budget_bytes=20000000000 window_rows=2375000000 bytes_per_row=8
                   safety_pct=95 window_target_bytes=19000000000000
                   scratch_margin_bytes=53687091200 local_waves=6 global_waves=53
                   local_rows_est=2602888457 projected_passes=2

# the forced-local arm (--subdiv-strategy=local): every window, measured
SS_PLAN waves=59 local=59 global=0 ...
SS_SUBDIV_POSBUDGET ... local_waves=59 local_rows_est=99964889630 projected_passes=43
SS_SUBDIV_POSSPACE window=[0,1) free_bytes=8504708698112 needed_bytes=9670885936
                   margin_bytes=53687091200 budget_bytes=20000000000 ok=1
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=100000000000 positions=1209029212
           position_bytes=9672233696 rows_est=1208860742 wall=7.62
SS_SUBDIV_POSPEAK window=[0,1) waves_n=1 bytes_written=9672233696 bytes_at_descent=9672233696
                   bytes_after_unlink=0 peak_total=9672233696 peak_window=0 budget_bytes=20000000000
SS_SUBDIV_POSPEAK window=[1,2) waves_n=1 bytes_written=18420133464 bytes_at_descent=18420133464
                   bytes_after_unlink=0 peak_total=18420133464 peak_window=1 budget_bytes=20000000000
SS_SUBDIV_POSPEAK window=[3,4) waves_n=1 bytes_written=16110831392 bytes_at_descent=16110831392
                   bytes_after_unlink=0 peak_total=18615853848 peak_window=2 budget_bytes=20000000000
SS_SUBDIV_POSPEAK window=[4,5) waves_n=1 bytes_written=18245565160 bytes_at_descent=18245565160
                   bytes_after_unlink=0 peak_total=18615853848 peak_window=2 budget_bytes=20000000000
```

**The number the fix is about.** By the end of window 4 the run had written
9.67 + 18.42 + 18.62 + 16.11 + 18.25 = **81.07 GB** of position files. The pre-fix
code held all 81.07 GB live (and would have kept growing to `local_rows x 8` =
800 GB at this corpus). `peak_total` stopped at **18.62 GB = one window**, and the
second window's peak (18.42 GB) does not contain the first window's 9.67 GB — every
window's `bytes_after_unlink=0` says the column was empty again before the next
window was sown.

P1 (peak ≤ budget) ✓ 18.62 GB ≤ 20 GB. P2 (sweep arithmetic) ✓
`projected_passes=43` = `ceil(99,964,889,630 x 8 / 19e9)` = `ceil(local_rows x 8 /
pos_budget)`; measured `SS_SCATTER passes` advances one per window (`pass=1` for
window `[0,1)`, `bytes_read=100000000000` = exactly one corpus sweep). P3 (no
re-scatter inside a window) ✓ `window_fallback_sweeps=0`: the only corpus sweeps are
the windows themselves (`corpus_sweeps_scatter == windows`) plus the plan's own
global classes.

The knob's shape, journaled end to end — the plan's wave goal is capped by the
position budget only when the budget is the binding term:

| pos_budget | wave_goal (journaled) | binding | local_rows_est | projected / measured passes |
|---|---|---|---|---|
| 20 GB (100 GB head) | 2,375,000,000 | `position-budget` | 2,602,888,457 (auto) | 2 |
| 20 GB, forced local | 2,375,000,000 | `position-budget` | 99,964,889,630 | 43 projected, 1 per window measured |
| 2 TB (relaunch value, journaled then stopped) | 2,926,829,260 | `band-plan` | 1,020,716,573 | 1 |

At 2 TB the wave goal is the band plan's own number again: the position budget
stops shaping the plan, which is the relaunch's regime.


### D3/P5 10 GB (`pf-10gb.txt`, N = 1e10 + 1), both arms, pre-fix vs post-fix

Walls below were taken with two 100 GB sorter arms running on the same box
(loaded-box numbers; the pre-fix binary has no interleave and the post-fix one does,
so a wall comparison here is not a speed claim — it is a correctness gate).

```
SUBDIV_DISK_D3 arm=auto  base_sha=a37dbe2a3d1fbb99ec2bf17f821fdb0d284c0279b92d89a5de7a5cc5c6efc3db
                new_sha=a37dbe2a3d1fbb99ec2bf17f821fdb0d284c0279b92d89a5de7a5cc5c6efc3db equal=1
                rows=10000000001 base_wall_s=625 new_wall_s=732 local_waves=0 corpus_sweeps_scatter=0 windows=0
SUBDIV_DISK_D3 arm=local base_sha=a37dbe2a3d1fbb99ec2bf17f821fdb0d284c0279b92d89a5de7a5cc5c6efc3db
                new_sha=a37dbe2a3d1fbb99ec2bf17f821fdb0d284c0279b92d89a5de7a5cc5c6efc3db equal=1
                rows=10000000001 base_wall_s=532 new_wall_s=795 local_waves=31 corpus_sweeps_scatter=5 windows=5
SUBDIV_DISK_D3_PASS
```

The same container sha in all four runs — and identical to the *forced-local* arm,
which is the arm that materialises 5 position windows — so the interleave is
byte-neutral at 1e10 rows. The `local` arm's receipt:

```
SS_PLAN waves=31 local=31 global=0 ...
SS_SUBDIV_POSBUDGET ... local_waves=31 local_rows_est=9996450373 projected_passes=5
SS_SUBDIV_POSPEAK window=[0,6)   waves_n=6 bytes_written=17990550904 bytes_after_unlink=0 peak_total=17990550904
SS_SUBDIV_POSPEAK window=[6,13)  waves_n=7 bytes_written=18791362512 bytes_after_unlink=0 peak_total=18791362512
SS_SUBDIV_POSPEAK window=[13,20) waves_n=7 bytes_written=16160821504 bytes_after_unlink=0 peak_total=18791362512
SS_SUBDIV_SWEEPS corpus_sweeps_scatter=5 corpus_sweeps_global_arm=0 total_corpus_sweeps=5
                 windows=5 window_fallback_sweeps=0 local_waves=31 global_waves=0
SS_SUBDIV_SCATTER passes=5 bytes_read=50000000000 positions=9999999977 windows=5
                  peak_total=18791362512 peak_window=6 position_dir_final_bytes=0
```

`positions=9,999,999,977` x 8 B = **80.0 GB of position files written**, peak live
**18.79 GB**, final directory **0 bytes**. P2's arithmetic is exact here:
`local_rows_est x 8 / 19e9 = 4.2` → `projected_passes=5`, measured 5. P3: 5 sweep
windows, 0 fallback sweeps inside them.

### D4 — the budget bounds the peak, the bytes are unchanged (1 GB budget)

Same 10 GB corpus, same binary, `--subdiv-policy` unchanged, position budget 1 GB
(vs 20 GB in the D3 local arm). The plan responds by cutting MORE waves, which is
the whole mechanism:

```
SS_PLAN waves=131 local=123 global=8 ...      (was waves=31 local=31 at a 20 GB budget)
SS_SUBDIV_WAVEGOAL wave_goal=118750000 band_goal=2926829260 binding=position-budget
SS_SUBDIV_POSBUDGET position_budget_bytes=1000000000 window_rows=118750000
                   window_target_bytes=950000000 local_rows_est=10890416153 projected_passes=92
SS_SUBDIV_SWEEPS corpus_sweeps_scatter=100 corpus_sweeps_global_arm=8 total_corpus_sweeps=108
                 windows=100 window_fallback_sweeps=0 local_waves=123 global_waves=8
SS_SUBDIV_SCATTER passes=100 bytes_read=1000000000000 positions=9106058419
                  position_budget_bytes=1000000000 windows=100 peak_total=959979976
                  peak_window=9 position_dir_final_bytes=0
VERIFY pos_budget=1000000000 peak_total=959979976 budget_ceiling=1050000000 windows=100
       scatter_passes=100 position_files_left=0 position_bytes_left=0
       container_sha=a37dbe2a... equal=1
VERIFY_DONE SS_DONE n=10000000001 waves=131 ties=1979147430 waves_s=1357.11 total_s=1365.13
VERIFY_PASS
```

* **P1** peak_total = **0.96 GB ≤ 1 GB** (budget ceiling with 5 % slack 1.05 GB) while
  the run wrote 9,106,058,419 x 8 B = **72.8 GB** of position files through 100
  windows; every window's `bytes_after_unlink=0`.
* **P2** 100 windows = 100 corpus sweeps of 10 GB = 1.0 TB read, one sweep per
  window, matching `ceil(local_rows x 8 / 0.95 GB) = 92` projected vs 100 measured
  (the plan cut more waves than the projection assumed, so a few windows hold one
  small wave each — the count is the plan's, the rule is unchanged).
* **P3** `window_fallback_sweeps=0`: the 8 global-arm sweeps are the plan's own
  sliced (point-mass tail) classes, not re-scans of a window.
* **P5** container sha **identical** to the 20 GB arm and to the pre-fix binary's:
  the budget changed how many windows there are, and nothing about the bytes.
* **P6** `SS_DONE`, `subdiv_pos` 0 files / 0 bytes, the whole scratch tree 0 bytes
  after exit, no survivors.

The 1 GB peak is the proof that the peak tracks the BUDGET and not the corpus: the
same corpus at a 20 GB budget peaked at 18.79 GB, at 1 GB it peaks at 0.96 GB, and
the container is the same file in both cases.

### The governor's own numbers (a full `xsa build --tape` fixture, 64 MB)

`gates/forward_only_f6_build.sh` (fixture corpus + independent docs map, so this is
the real pipeline, not the sorter alone) — the Rust change is exercised end to end
and the container certifies:

```
SCRATCH_GOVERNOR budget_gb=8.000 pos_budget_gb=1.600 ceiling_gb=59.600 margin_gb=50.000
  pause_at_gb=8.000 resume_at_gb=7.200 producers=order-tape,emit-tape
  accounting=the_whole_scratch_tree_incl_subdiv_pos_excl_the_container
  rule=pause_on_the_budget,ratchet_never_past_the_ceiling,fail_past_the_ceiling
SCRATCH_SAMPLE live_gb=0.022 output_gb=0.000 budget_gb=8.000 peak_gb=0.022 paused=0
  allocated_bytes=1 pos_budget_gb=1.600 ceiling_gb=59.600
CONTAINER_CERTIFIED format=sxi2-v10 version=10 n=64004096 runs=63673072
  members=m1,m4,m5,m7,m10,m11,m12,m13,m15,m20
F6_PASS (query, mem, branch_of, trie_info, docstarts, stats, consult, seeds,
         build-without-docs-map refused, build-no-trie)
```

## 5. The relaunch

The shape the supervisor set, with the numbers this fix guarantees:

```
XSA_SUBDIV_POSITION_BUDGET_BYTES=2000000000000 \
XSA_SUBDIV_SCRATCH_MARGIN_BYTES=200000000000 \
xsa build --tape --input /mnt/nvme2n1/erikg/pile.txt --scratch PILE \
          --threads 96 --memory-gb 256 --scratch-budget-gb 250 \
          --trie-depth 8 --trie-min-support 1000 --docs-map pilemap_full
```

**The peak it holds to: `min(pos_budget, local_rows x 8)` = `min(2 TB, ~10.5 TB) =
~1.9 TB`** (the window is planned to 95 % of the budget), *not* 10.5 TB. What the
run will show, and what to check the journal against:

| journal line | meaning | expected |
|---|---|---|
| `SS_SUBDIV_POSBUDGET ... position_budget_bytes=2000000000000 window_rows=237500000000 safety_pct=95 window_target_bytes=1900000000000 scratch_margin_bytes=200000000000` | the knobs the run actually used (env, `source=env`) | must show 2 TB and 200 GB, not the defaults |
| `SS_SUBDIV_POSPEAK window=[...) bytes_written= bytes_at_descent= bytes_after_unlink=0 peak_total=` | one window on disc at a time; `peak_total ≤ 1.9 TB` | last window's `peak_total` is the number to compare to free disc |
| `SS_SUBDIV_SCATTER passes= windows= peak_total= position_dir_final_bytes=0` | the receipt: windows == passes, directory empty at exit | `position_dir_final_bytes=0` |
| `SS_SUBDIV_SWEEPS ... window_fallback_sweeps=0` | corpus-wide sweeps == one per window + the plan's own global classes | no sweep inside a window |
| `SCRATCH_GOVERNOR ... ceiling_gb=2248 ...` | the governor's model for this run | `ceiling_gb ≈ 2.2 TB` |
| `SS_SUBDIV_KNOBS ... source=env` | the knobs came from the environment | `source=env` |

Free disc at launch 7.8 TB > peak 1.9 TB + container (~860 GB) + bands (~250 GB) =
~3.0 TB. `subdiv_pos` must be empty (`find PILE/waves-tape/subdiv_pos -type f | wc
-l` → 0) once the sorter finishes, and *during* the run its size is the live
position of ONE window, not a growing column:

```
watch -n 30 "du -sb /mnt/nvme2n1/erikg/PILE/waves-tape/subdiv_pos"
```

The knob's shape, which is the whole cost model — `sweeps = ceil(local_rows x 8 /
pos_budget)`, one full corpus sweep per window, and nothing else:

| pos_budget | sweeps at the pile (local_rows x 8 ≈ 10.5 TB) | peak | extra reads |
|---|---|---|---|
| 2 TB (the relaunch) | 6 | ~1.9 TB | 6 x 1.31 TB ≈ 8 TB ≈ 10 min at the 13 GB/s these sweeps measure |
| 500 GB | 21 | ~475 GB | 21 x 1.31 TB ≈ 35 min |
| 20 GB | 524 | ~19 GB | 524 x 1.31 TB ≈ 2.4 days of sweeping — do not |

## 6. What is NOT in this fix: the DFS-inside-a-window optimisation

The supervisor's depth-first rule (materialise a node's position file, stream it
into its children's files, `fallocate(FALLOC_FL_PUNCH_HOLE)` the consumed prefix,
unlink the parent) is the right shape for the descent **below the window's roots**,
and it is a **named follow-up, not part of this fix**:

* **benefit**: inside a window, the deep classes would be produced by streaming the
  parent's file instead of requiring the window's classes to be re-derived per
  level — zero re-scatter for the deep nodes (today: zero too, because the plan's
  waves are the leaves and one sweep per window seeds all of them; the DFS version
  would keep that property with a peak of one *node* rather than one window);
* **cost**: a descent rewrite — a plan tree (today the plan is a flat, key-ordered
  wave list), per-node position files, a streaming subdivider, punch-hole
  bookkeeping, and the leaf's file-to-band plumbing;
* **why it cannot be the top-level fix**: the first materialisation still has to
  write every top-level class's positions in one sweep, i.e. `N x 8 B` = 10.5 TB at
  the pile, and the pile's largest single class (space, 2.17e11 rows) is 1.7 TB on
  its own — *larger* than the 1.9 TB a 2 TB window holds only when the window is
  smaller than the class. A budget-bounded window is therefore the correct
  invariant at the top level, and the DFS buys re-scatter, not a lower root peak.

## 7. The false claim, corrected

`RESULTS_SUBDIVISION.md` said: *"The disk is free to ~8 TB on the pile box, and the
position files of one window are unlinked as soon as the window's waves are
descended."* The second clause was false in the shipped code (the unlink existed
inside the per-wave descent, but the scatter loop had already written **every**
window before the first descent, so the peak was the whole column). It is true now
only because the window is sown lazily — the sentence has been rewritten to say that,
and to point at `SS_SUBDIV_POSPEAK` as the number that proves it.

`WAVE_SUBDIVISION.md` §"Positions" (\(a bin's positions are held in rolling files,
ascending, so every level is a linear walk\)) is unchanged and correct; a
CORRECTION note has been appended recording that the *lifetime* it implies (one
window at a time) is what the implementation now enforces, with the gate that
measures it.

## 8. Residual risks

1. **The peak still scales with `local_rows`, bounded by the budget.** With the
   relaunch's 2 TB the run holds ≤ 1.9 TB of positions; a corpus whose local rows
   are small keeps the peak small, but a corpus with more local rows than
   `budget / 8` costs exactly `ceil(local_rows x 8 / budget)` corpus sweeps. The
   knob trades disk for passes and nothing else changes.
2. **`statvfs` sees the filesystem, not the driver's plans.** Free space can shrink
   under the window (another lane, the finish's own growth). The margin (200 GB at
   the pile) is the answer, and the governor's ceiling is the backstop; neither can
   make a shared box's free space atomic.
3. **`rowsEst` is a sample estimate.** Windows are planned to 95 % of the budget to
   absorb it (G8: ≤ 0.74 % error) and the writer dies rather than overrun — a
   legitimate run therefore cannot silently exceed the budget, but a corpus whose
   estimates are much worse than measured would refuse instead of shrinking.
4. **The strategy rule still chooses global for most classes.** At the 100 GB head
   the plan picks `local=6 global=53`: the position column only covers the sparse
   classes, and the global ones cost one sweep each (unchanged by this fix, and
   priced by `SS_SUBDIV_SWEEPS`). A tighter budget cannot improve that; only the
   prefix cache's depth and the page unit can.
5. **`chain.rs` governor changes are unit-scale, not yet exercised end to end.** The
   ceiling/ratchet paths are journaled and compiled, and the sorter-side guards are
   the ones the gates exercise; the governor's kill path is not reachable from a
   sorter-only harness. It is exercised by the pile relaunch itself, which prints
   `SCRATCH_GOVERNOR` at start so the model is checkable before the first window.
