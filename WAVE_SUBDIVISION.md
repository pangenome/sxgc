# Wave subdivision: survey → plan → descent

The spec for the sorter's descent below the top-level bins. It replaces the
wave layer's point-mass/borrow/refuse ladder. Status: **spec, not yet
implemented** (the pile build is blocked on it).

## The defect this fixes

At 100 GB the wave layer worked; at 1.31 TB it refused:

```
SS_WAVE_POINT_MASS depth=16 class_rows=9,863,915,159 wave_goal=3,771,190,579
SS_WAVE_POINT_MASS_SPLIT depth=28 largest_child_rows=7,361,560,178 children=138
SS_WAVE_BORROW arena_gb=826.784 mem_cap_gb=238.419 path=refuse
```

Three measured causes, all in `xsa/runtime/tools/binbwt/scatter_sort.cpp`:

1. **A hard depth cap.** `SS_WAVE_DEPTH_MAX = 16` with a fixed
   `unsigned char pre[16]` in the `Wave` struct. At depth 16 the descent could
   not hold another key byte, so the class was declared a "point mass".
2. **Every level cost a global sweep.** The wave loop `pread`s the whole corpus
   and tests `wave_match(p)` for every `p` in `0..n`. At 100 GB that was 102
   waves in 12 h (≈7.4 min/wave); at the pile each wave is a 1.31 TB read.
3. **Every census touched the corpus.** Class censuses call `sym_at(p+d)` over
   the positional sample. At the pile the stride is ~327 KB, so each sample read
   is a fresh page: ~4 M scattered reads per level, on top of the sweeps.

Nothing in the descent was wrong about *radix*; it was paying global prices for
local work, and it stopped at an implementation constant instead of at a target.

## The doctrine

**Global work is O(1) passes. The descent is local. Never a merge.**

* A new top level legitimately costs a global pass: the count pass plus the
  scatter pass collect **all** top-level bins in one sweep (per-thread regions,
  no lock, no shared cursor). That is the only global corpus scan.
* Division below the top level costs `|class|`, never `n`.
* Depth is data; there is no depth cap.
* Buckets are concatenated in key order. A merge is never introduced, and no
  unproved order is ever published.

## Survey → plan → descent

```
SURVEY   one sweep, collecting everything the planning will ever need
PLAN     in memory, answerable for any class at any depth, zero corpus reads
DESCENT  the tape passes: they ROUTE positions into bins, guided by the plan
```

### 1. Survey (one pass, deterministic, journaled)

| collected | size at the pile | used for |
|---|---|---|
| exact byte counts per chunk per top-level bin | existing | top-level bins |
| prefix cache: next `P` bytes at each sampled position | 4 M × 128 B = 512 MB | class census at depth ≤ P |
| run-length column: run length of the symbol at each sample (u32, capped) | 16 MB | run-length censuses |
| page column: the 1 MiB page each sample occupies | derived | `coverage(class)` |

Journal `SS_SURVEY samples= stride= prefix_bytes= run_cap= pages= sha256=` and
keep the blob under ~0.5 GB. The survey is a **sample**: everything it says is
an estimate, labelled as one. Exactness comes from the descent's own collection
of the class's positions.

### 2. Plan (in memory)

For any class and depth `d ≤ P`, without touching the corpus:

* `rows(class)`, `children(class, d)` — from the prefix cache;
* `run_lengths(class)` — from the run-length column;
* `coverage(class) = #distinct 1 MiB pages among sampled class positions × stride / n`.

Depths beyond `P` fall through to windows for the (by then small) class and
journal `SS_PLAN_CACHE_MISS depth= class_rows=`.

### 3. Descent (the passes)

A class is subdivided until it is a **leaf**. Two decisions come from the plan:

**Strategy, per class:**

```
coverage <  0.25  → local : stream corpus windows around the class's positions
coverage >= 0.25  → global: one sequential sweep, routing the class's positions as
                             the cursor passes them, resolving several digits at once
```

Both strategies must produce **byte-identical** partitions — the strategy is a
cost decision, never a semantic one. Journal
`SS_SUBDIV_STRATEGY class= rows= coverage= strategy= bytes_read=`.

**Leaf criterion, in the currency the sort pays:**

```
leaf  ⇔  rows <= target_rows  AND  coverage <= 0.05 … 0.10
```

Rows are a proxy; pages are what the leaf sort costs. A leaf whose rows still
touch a large fraction of the corpus makes the inner suffix sort fault the whole
file — exactly the cost the descent exists to remove. Journal each leaf's
expected page footprint.

**Termination and the pathological case.** Children shrink monotonically, so
coverage falls with depth for ordinary text. For corpora where a long chunk
repeats everywhere, coverage refuses to fall: detect the stall
(`SS_SUBDIV_COVERAGE_STALL passes= coverage= rows=`) and take the existing
prefix/cheap-pricing lever — never more passes, never a merge.

**Positions.** A bin's positions are held in rolling files (the existing
window-file machinery: bounded, preallocated, completion-word protected),
ascending, so every level is a linear walk.

## Gates

Every gate prints N of things examined. A vacuous check is the project's killer
pattern; a proof or a plan that costs a global pass per class is the same defect
wearing a proof's clothes.

| gate | asserts |
|---|---|
| G1 fixture parity | band stream byte-identical between the borrow arm and this path (N rows, N bands) |
| G2 brute oracle | exact SA on small fixtures vs emitted order: EOF runs, doc-boundary terminators, internal 0x1E, r=0, mixed families (N cases) |
| G3 no global rescan | global scans after the scatter = 0 unless `strategy=global`; local `bytes_read` independent of `n` (same class at 10 GB and 100 GB head) |
| G4 100 GB head | the head's space class (762,642,500 rows) through both arms, byte-identical band streams; waves, per-level reads, wall |
| G5 refusal | a class the census cannot prove still refuses, with numbers; never a merge |
| G6 strategy equivalence | forced `--subdiv-strategy=local|global` produce identical band-stream shas; coverage estimate validated against the exact count |
| G7 survey reproducibility | two surveys produce identical `SS_SURVEY` lines and blob sha |
| G8 plan == reality | predicted counts/coverage vs exact values from the class's full position list, with the error distribution printed (N classes) |
| G9 no census reads | corpus reads during PLAN = 0; during DESCENT only window reads for depths > P |

## What is explicitly out of scope

* Any merge, anywhere, for any reason. The refusal says so today and it stays.
* Removing the one-wave borrow path for a provably single-symbol tail run.
* Changing the container format, the tape band protocol, or the intake.

## CORRECTION (2026-11-14): the LIFETIME of a window's position files

§"Positions" above says a bin's positions are held in rolling files, ascending, so
every level is a linear walk. That is the data shape and it is correct. What the
implementation got wrong is the **lifetime**: `scatter_sort.cpp` wrote every
window's position files before the wave loop descended the first of them, so the
peak was `local_rows × 8 B` — ~10.5 TB at the pile, a corpus number that no
position budget could lower — and the pile run filled its filesystem at 7.6 TB of
`subdiv_pos` against a 250 GB scratch budget.

The rule the implementation now enforces (`fix/subdiv-window-disk`):

* a window's positions are written **immediately before** that window's waves are
  descended, and unlinked **after the window's last wave** — one window live at a
  time, measured and journaled as `SS_SUBDIV_POSPEAK window= bytes_written=
  bytes_at_descent= bytes_after_unlink= peak_total=`;
* a window may not exceed `--subdiv-position-budget-bytes`: `waveGoal` is capped at
  `budget × 95 % / 8` so no local wave can need more than a window holds, and the
  writer counts the bytes it writes and refuses to exceed the budget;
* before a window is written, `statvfs` must show the window plus a margin free on
  the scratch filesystem, or the run DIES with free/needed/margin in the message.

The gates that measure this (P1–P6) are in `RESULTS_SUBDIV_DISK.md`, together with
the depth-first-within-a-window optimisation that remains a follow-up.
