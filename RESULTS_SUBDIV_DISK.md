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

### D2/P1–P3 100 GB head, pos_budget 20 GB

<!--P1-->

### D3/D4/P5–P6 10 GB, both arms, pre-fix vs post-fix

<!--P5-->

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
