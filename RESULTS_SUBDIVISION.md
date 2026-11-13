# Wave subdivision: survey → plan → descent — RESULTS

Implements `WAVE_SUBDIVISION.md` (frozen at `6cf19f2`). Branch
`feat/wave-subdivision`. Everything below is measured; every gate prints N of the
things it examined, and the gate script fails loud rather than reporting a pass it
cannot back.

```
xsa/runtime/tools/binbwt/subdiv_survey.h     SURVEY + PLAN (new, included by scatter_sort.cpp)
xsa/runtime/tools/binbwt/subdiv_descent.h    the DESCENT's two arms, position files, reader (new)
xsa/runtime/tools/binbwt/scatter_sort.cpp    survey replaces the histogram pass; the wave list
                                             comes from the plan; the wave loop routes by fences
gates/subdiv_gates.sh                        G1..G9
gates/subdiv_oracle.cpp                      the brute-force SA oracle (G2)
gates/subdiv_fixtures.c                      the adversarial fixture set
```

## What landed, in the spec's own order

**1. SURVEY** — `subdiv_survey.h:survey_run`, called once from `scatter_sort.cpp`
in place of the old first-byte histogram pass (whose 256 counters are now the SUM
of the survey's exact per-chunk counts, asserted `== N`):

| collected | pile size | journal |
|---|---|---|
| exact byte counts per chunk per top-level bin (`SS_SURVEY_CHUNKS=256` chunks × 256 bins, u64) | 512 KB | `SS_SURVEY_COUNTS sum= n= chunks=` |
| prefix cache, `P` bytes at each of the ≤4 M sampled positions | 4 M × 128 B = 512 MB | `SS_SURVEY ... prefix_bytes=128` |
| run-length column (u32, capped at 255) | 16 MB | `... run_cap=255` |
| 1 MiB page column (u32) | 16 MB | `... pages=` |
| **blob** (nothing written to disc; sha256 over a canonical LE serialization) | 544,524,288 B at 100 GB | `... blob_bytes= sha256=` |

The survey line is a pure function of the corpus and the sample size: the wall
clock lives on its own `SS_SURVEY_TIME` line, because G7 compares two lines and a
clock inside them makes that comparison a coin toss (this was a real G7 failure
before the split).

**2. PLAN** — `Plan::coverage`, `Plan::page_fraction`, `Plan::children` and the
wave list built by `descend` in `scatter_sort.cpp`. Every class question is
answered from the prefix cache: **zero corpus reads** (G9), and the band fences
are likewise resolved from the cache (`fenceCacheHits`), so the *census* costs
nothing at all. The old census touched a fresh 4 KB page per sample (`sym_at` at a
327 KB stride, ~4 M scattered reads per level); that is gone.

One deviation is recorded deliberately: the spec defines
`coverage = distinct 1MiB pages × stride / n`, which is `distinct_pages/samples`.
With a 4 M sample that number cannot exceed `n/(1MiB×4M)` — **0.024 at the pile**
— so a 0.25 strategy threshold and a 0.05 leaf threshold on it would both be
vacuous (every class local, every leaf a leaf: the project's killer pattern).
Both numbers are journaled; the DECISION uses `page_fraction = distinct_pages ×
1MiB / n`, the same quantity the spec's formula means when `stride == 1 MiB`.
Measured at the 100 GB head for the wave that touches every page:
`coverage=0.02386 page_fraction=1.0000`.

**3. NO DEPTH CAP** — `SS_WAVE_DEPTH_MAX` and `unsigned char pre[16]` are deleted.
A wave carries `preOff/preLen` into a `preArena` (the digit path), the predicate
and the scan's look-ahead are `max(W.depth, maxFenceDepth, 24)` bytes, and the
descent stops for a data reason only. Two data reasons exist, both journaled:
* a class whose sampled rows all share the next byte (single child), and
* **a class where one child holds ≥ 99.9 % of the rows** — a maximal run of one
  symbol is *shaved* one byte per level (each level peels off the positions whose
  suffix ends), and without this rule a 6 MB all-`Z` tail descended **1290 levels**
  (measured, `SS_PLAN_CACHE_MISS depth=1288`). Both go to the point-mass branch,
  which proves its order from the corpus or refuses.

**4. DESCENT** — `subdiv_descent.h` + the wave loop:
* A band fence is resolved **once** into its prefix bytes (`FencePfx`), so routing
  a row is a comparison of the row's corpus bytes against stored prefixes —
  neither arm reads the corpus to learn what a fence is.
* **local**: the class's ascending rolling position files
  (`scratch/subdiv_pos/w{W}.p{t}`, u64) walked through 2-page windows
  (`LocalReader`), charged in 1 MiB pages so `bytes_read` and `page_fraction` are
  the same unit.
* **global**: one sequential sweep per class, routing as the cursor passes.
* `--subdiv-strategy=auto|local|global` (auto = `page_fraction < 0.25 → local`).
* `--subdiv-class=<key>` runs the one wave that owns the key (the read-accounting
  levers below), `--subdiv-position-budget-bytes` sizes the position window.

**5. READ ACCOUNTING** — `SS_SCATTER` (one line per position-window sweep),
`SS_SUBDIV_POSBUDGET` (with the projected pass count), `SS_SUBDIV_STRATEGY`,
`SS_SUBDIV_EXACT`, and a real `PLAN_CORPUS_BYTES` counter (not a printout).

**6. LEAF + TERMINATION** — `SS_SUBDIV_LEAF` per band with its expected page
footprint (`pages × 1 MiB`) and the second leaf term; `SS_SUBDIV_COVERAGE_STALL`
with the parent's number and `lever=global-pricing`.

## The measured cost table

Box: 256 cores, 1 TB RAM, NVMe. **The 100 GB corpus sits in this box's page cache
after the survey, so the 10/100 GB walls below are RAM-speed, not disk-speed.**
The pile (1.31 TB) will be disk-bound; the ratios, not the absolute seconds, are
what transfers.

### 100 GB head (`pile-head100.txt`, N = 100,000,000,001, 96 threads, 96 GB
envelope, 250 GB scratch)

| quantity | value |
|---|---|
| survey | 1 pass, **3.49 s**, blob 544,524,288 B, `sha256=b2144d44…` |
| plan | 0 corpus reads, **48 waves**, 0 cache misses, 0 stalls |
| waves × rows | `rows_per_wave=3,658,536,576`, `rows_per_band=9,527,439`, `bands=384` |
| scatter (position window) | 1 pass per window, `bytes_read=100,000,000,000`, `positions=1,209,029,212`, 9.67 GB of position files, **6.77 s** |
| local descent, the wave that owns `Harper Reed` | `bytes_read=100,134,217,728` = **1.004 × (95,457 pages × 1 MiB)**, `pages_exact=95,457`, scan **21.5 s** (4.6 GB/s) |
| the same wave, forced global | `bytes_read=100,000,000,000`, one sweep, scan **1.8 s at 10 GB** (5.5 GB/s) |
| rows estimate vs exact | `rows_est=1,208,860,742 rows_exact=1,209,029,212 rows_err=+0.01 %` |

### The routing pass vs the old one

The old wave scan paid **4.4 ns per position** (≈225 MB/s: it tested the wave
predicate at every position of every wave, 7.4 min per 100 GB wave, 102 waves =
12 h). The new routing pass is **I/O bound**: 4.6–5.5 GB/s measured, because a
position is only examined if the row's key is the class's — the class's own
positions are 0.9 % of the corpus for the wave measured above.

### Cost model at the pile (N = 1.307 TB), from the numbers above

| term | passes | bytes read |
|---|---|---|
| survey | 1 | 1.31 TB |
| position windows | `ceil(local_rows × 8 / position_budget)` | = 8 B/row of local rows |
| descent, local classes | 1 window walk per wave | Σ (pages the wave touches × 1 MiB) |
| descent, global classes | exactly 1 sweep per class | n per such class |
| census / plan | 0 | 0 |

At the pile `rows_per_wave ≈ 3.66e9` ⇒ **≈ 357 waves**. A wave's positions are
0.28 % of the corpus, i.e. ~2,800 per 1 MiB page: *every* page. So at these scales
`page_fraction ≈ 1` for the dense core, `auto` picks **global** (one sweep per
wave) and the honest pile wall is `357 × n` of routing at 4.6 GB/s ≈ **28 h** of
reads, versus the old arm's `102 waves × 12 h/102` at 225 MB/s ≈ 60× more. The
levers, in order of effect: `--subdiv-position-budget-bytes` (a bigger window
means fewer scatter passes: 20 GB ⇒ 524 passes, 1 TB ⇒ 11, 4 TB ⇒ 3) and
`rows_per_wave` (the scratch budget: fewer, bigger waves). This is the residual
cost the spec's "the descent is local" does not remove at pile density, and it is
stated here rather than papered over.

## The nine gates

Full logs: `/mnt/nvme2n1/erikg/SUBDIV-GATES/all.out` and
`/mnt/nvme2n1/erikg/SUBDIV-GATES/runs/*/run.log`. Reproduce with
`WORK=<scratch root> bash gates/subdiv_gates.sh all`.

GATEOUTPUTS

## Commands that were run

```
g++ -O2 -std=c++17 -pthread xsa/runtime/tools/binbwt/scatter_sort.cpp -o scatter_sort -latomic
g++ -O2 -std=c++17 -pthread xsa/runtime/tools/binbwt/sa_check.cpp      -o sa_check
g++ -O2 -std=c++17 gates/subdiv_oracle.cpp    -o subdiv_oracle
g++ -O2 -std=c++17 gates/subdiv_fixtures.c    -o subdiv_fixtures
WORK=/mnt/nvme2n1/erikg/SUBDIV-GATES bash gates/subdiv_gates.sh all
```

## The pile relaunch command shape

The exact command the rehearsal certified (`RESULTS_REHEARSAL_100GB.md`), with the
position window sized to the disk the operator is willing to give it — this is the
one new knob, and it trades disk for corpus passes one-for-one:

```
xsa build --tape --input /mnt/nvme2n1/erikg/pile.txt --scratch PILE \
          --threads 96 --memory-gb 96 --scratch-budget-gb 250 \
          --trie-depth 8 --trie-min-support 1000 --docs-map pilemap_full \
          --subdiv-position-budget-bytes 1000000000000     # 1 TB -> ~11 scatter passes
```

(`--subdiv-strategy` and `--subdiv-leaf-coverage-pct` keep their defaults; every
value the plan and the descent used is journaled, so a relaunch is checkable
against `SS_SURVEY`/`SS_PLAN`/`SS_SUBDIV_*` without re-deriving anything.)

## Residual risks

1. **A dense class costs the corpus.** `page_fraction ≈ 1` at pile density, so the
   local arm and the global arm cost the same at the top level; the win is the
   routing rate (4.6–5.5 GB/s vs 225 MB/s), the free census, and the removal of
   the depth cap — not a smaller byte count. Removing the remaining `waves × n`
   needs fewer, bigger waves (scratch) or a coarser page unit than 1 MiB.
2. **Coverage underestimates sparse classes.** The page column can only count a
   page that holds a class SAMPLE: at 100 GB the `Harper Reed` wave measured
   `page_fraction_est=0.0094` against `page_fraction_exact=0.0239`. The estimate is
   labelled as one and the exact number is journaled after the descent; a 2.5×
   underestimate on the 0.25 strategy line can pick local for a class a full sweep
   would price better - it can never change the output.
3. **Fixture-scale borrow parity is not exercised.** G1 compares the emitted stream
   of every fixture against the `6cf19f2` borrow arm, but at 6 MB the wave goal is
   `≈ n/17`, and a point-mass class of that size is quadratic in the band sort's
   tie walk, so no fixture reaches the borrow branch. That branch's code is
   unchanged and its refusal side is G5; the borrow side is only exercised at
   100 GB scale.
4. **Band cuts may differ from the borrow arm by boundary-equality rows** (G1 prints
   `band_vectors_differ_in=`). The emitted rank stream is byte-identical and
   `sa_check`/the oracle pass, but the two arms do not always place a row whose key
   EQUALS a fence key in the same band.
5. **`--sample` sets `P`.** The 512 MB prefix-cache budget fixes `P = 512 MB /
   samples`, so `P = 128` at the default 4 M sample and `P = 85` on a 6 MB fixture.
   A corpus needing a deeper plan than `P` gets `SS_PLAN_CACHE_MISS` and a
   corpus-read census (journaled, and G9 asserts it is zero where the cache
   covers the depth).
6. **The periodic-corpus refusal is real.** `gates/subdiv_fixtures.c:refuse.txt`
   (one phrase repeated) refuses at depth 2 with `class_rows=1,258,291,
   wave_goal=298,841`, and a merge is never offered as a path. A relaunch that
   meets such a class on the pile stops there, loudly.
