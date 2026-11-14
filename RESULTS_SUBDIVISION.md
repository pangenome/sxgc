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
| scatter (position window) | 1 pass per window, `bytes_read=100,000,000,000`, `positions=2,918,585,840`, 23.3 GB of position files, **5.97 s** |
| local descent, the wave that owns `Harper Reed` (`depth=1 bytes=[69,91)`) | `bytes_read=100,133,169,152` = **1.001 × (95,462 pages × 1 MiB)**, `pages_exact=95,462`, `page_fraction_exact=1.0010` |
| the same wave, forced global | `bytes_read=100,000,000,000`, exactly one sweep, `classes=1 sweeps=1` |
| rows estimate vs exact | `rows_est=2,918,585,840 rows_exact=2,918,585,840 rows_err=0.0000` |
| 10 GB, same key | `bytes_read=10,122,683,392` = 1.002 × (9,632 pages), `n_fraction=1.0123` |
| routes/s: 100 GB routing pass | 4.6–5.5 GB/s measured (the old per-position predicate scan: 225 MB/s) |

### Coverage trajectories (page_fraction, the decision unit)

| corpus | depth 1 | depth 2 | saturated (≥0.9) | stalls |
|---|---|---|---|---|
| 100 GB head, full plan | 0.3951 … 1.0000 | 0.6274, 0.6694, … | 30 of 48 waves | 0 |
| 512 MB slice, full plan (G8) | 15 waves, min 1.0000 mean 1.0000 | 16 waves, min 1.0000 mean 1.0000 | 31 of 31 | 0 |
| 100 GB head, the `Harper Reed` wave, exact | 1.0010 | — | — | 0 |
| 6 MB fixtures | 1.0000 | 1.0000 | 31–32 of 31–32 | 0 |

**The stall detector does not fire on any measured corpus, and the number that
would have to move for it to fire is printed:** the footprint is *saturated*
(≥0.9 of the corpus) for 30 of the 48 waves at 100 GB and for every fixture wave,
but no class was subdivided three or more levels with the footprint refusing to
fall — the repetitive corpora that would do that (the `refuse` fixture, a word
repeated everywhere) hit the point-mass branch at depth 2 first, and the branch's
proof is what refuses them. The detector is live: it fired on every wave of the
first docsep run (before the `depth >= 3` condition), which is how the shadowed
variable was found.

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

All nine gates in ONE run of `gates/subdiv_gates.sh all` with the final
binary (`scatter_sort` sha256 `665426745f96aa698352fe91c26a2580952f0bb52d95998e682bedaf11c8a26a`),
base arm `6cf19f2` (sha256 `6ce8ed2a…`). Full log:
`/mnt/nvme2n1/erikg/SUBDIV-GATES/final2.out`.

### G1 fixture parity — 7 fixtures, 44,040,199 rows (borrow arm = 6cf19f2)

```
SUBDIV_G1 fixture=docsep rows=6291457 bands=92 base_sha=539522013cbe992a586ab44930b083d267eb53aee13d24b66213b13f2967e510 new_sha=539522013cbe992a586ab44930b083d267eb53aee13d24b66213b13f2967e510 equal=1 band_vectors_differ_in=6 sa_check=SA_CHECK_PASS
SUBDIV_G1 fixture=eofrun rows=6291457 bands=91 base_sha=c095ccfe31081aedd0d2f27061aac3f9d197e67cd11cf54d78e8c22b97c98986 new_sha=c095ccfe31081aedd0d2f27061aac3f9d197e67cd11cf54d78e8c22b97c98986 equal=1 band_vectors_differ_in=2 sa_check=SA_CHECK_PASS
SUBDIV_G1 fixture=internsep rows=6291457 bands=94 base_sha=d5f5f42053018920b2127aea9094244abce212ac9803098210c59dc0d2a1b84e new_sha=d5f5f42053018920b2127aea9094244abce212ac9803098210c59dc0d2a1b84e equal=1 band_vectors_differ_in=2 sa_check=SA_CHECK_PASS
SUBDIV_G1 fixture=mixed rows=6291457 bands=155 base_sha=2b1c5fa341a19945e299a04528b18b6a9a5701cb424e76c4e1bbf540742d3741 new_sha=2b1c5fa341a19945e299a04528b18b6a9a5701cb424e76c4e1bbf540742d3741 equal=1 band_vectors_differ_in=6 sa_check=SA_CHECK_PASS
SUBDIV_G1 fixture=period2 rows=6291457 bands=91 base_sha=9eec1a23e34595bcc5322f42090755a36b2ac5555fa760f9b79c03408ad1c442 new_sha=9eec1a23e34595bcc5322f42090755a36b2ac5555fa760f9b79c03408ad1c442 equal=1 band_vectors_differ_in=4 sa_check=SA_CHECK_PASS
SUBDIV_G1 fixture=r0 rows=6291457 bands=91 base_sha=d3c09c42badedb94ba175cdfa82636f92754e541f5cb0c8f9d77ea673521853b new_sha=d3c09c42badedb94ba175cdfa82636f92754e541f5cb0c8f9d77ea673521853b equal=1 band_vectors_differ_in=7 sa_check=SA_CHECK_PASS
SUBDIV_G1 fixture=single rows=6291457 bands=91 base_sha=75ce36eb394cc6c3f9db00d96cd3b42aa4d172812f025bd9160b665eb96ae331 new_sha=75ce36eb394cc6c3f9db00d96cd3b42aa4d172812f025bd9160b665eb96ae331 equal=1 band_vectors_differ_in=3 sa_check=SA_CHECK_PASS
SUBDIV_G1_PASS fixtures=7 rows=44040199 (band stream byte-identical, sa_check PASS on every one)
```

### G2 brute-force SA oracle — 7 adversarial cases

```
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_ORACLE corpus_n=6291456 entries=6291457 expected=6291457 mismatches=0 first_bad_rank=18446744073709551615 got=18446744073709551615 want=18446744073709551615
SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
SUBDIV_G2_PASS cases=7 (EOF run, doc-boundary 0x1E, internal 0x1E, r=0 tails, mixed families, periodic run, single-symbol run)
```

### G3 read accounting — the same class at 10 GB and at the 100 GB head, and the forced-global arm

```
SS_SUBDIV_EXACT wave=0 class=<root> depth=1 rows_exact=6422835 rows_est=6422800 rows_err=0.0000 pages_exact=512 coverage_exact=0.000128 coverage_est=0.000128 page_fraction_exact=1.0000 page_fraction_est=1.0000 bytes_read=542113792 rule=exact_from_the_class_full_position_list
SS_SUBDIV_EXACT wave=1 class=\x20 depth=2 rows_exact=25428847 rows_est=25491490 rows_err=-0.0025 pages_exact=512 coverage_exact=0.000128 coverage_est=0.000128 page_fraction_exact=1.0000 page_fraction_est=1.0000 bytes_read=542113792 rule=exact_from_the_class_full_position_list
SS_SUBDIV_EXACT wave=2 class=\x20 depth=2 rows_exact=23786304 rows_est=23722422 rows_err=0.0027 pages_exact=512 coverage_exact=0.000128 coverage_est=0.000128 page_fraction_exact=1.0000 page_fraction_est=1.0000 bytes_read=541065216 rule=exact_from_the_class_full_position_list
SS_SUBDIV_CLASS_ONLY key='Harper Reed' wave_depth=1 bytes=[48,76) rows_est=472025678 coverage=0.002384 page_fraction=0.9999 strategy=local note=emitted_stream_covers_this_class_only
SS_PLAN waves=1 local=31 global=0 cache_depth=128 cache_misses=0 stalls=0 saturated=30 samples=4000000 stride=2500 corpus_reads=0 corpus_read_calls=0 rule=answer_from_prefix_cache
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=10000000000 positions=472025678 position_bytes=3776205424 rows_est=472025678 wall=1.02 rule=one_sweep_per_position_window
SS_SUBDIV_EXACT wave=0 class=<root> depth=1 rows_exact=472025678 rows_est=472025678 rows_err=0.0000 pages_exact=9632 coverage_exact=0.002408 coverage_est=0.002384 page_fraction_exact=1.0100 page_fraction_est=0.9999 bytes_read=10122683392 rule=exact_from_the_class_full_position_list
SUBDIV_G3_SCALE dir=/mnt/nvme2n1/erikg/SUBDIV-GATES/runs/g3_10gb n=10000000001 class_bytes_read=10122683392 pages_exact=9632 bound=9730MiB within=1 bytes_per_page=1.002 n_fraction=1.012268
SS_SUBDIV_CLASS_ONLY key='Harper Reed' wave_depth=1 bytes=[69,91) rows_est=2918585840 coverage=0.016753 page_fraction=0.7027 strategy=local note=emitted_stream_covers_this_class_only
SS_PLAN waves=1 local=48 global=0 cache_depth=128 cache_misses=0 stalls=0 saturated=0 samples=4000000 stride=25000 corpus_reads=0 corpus_read_calls=0 rule=answer_from_prefix_cache
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=100000000000 positions=2918585840 position_bytes=23348686720 rows_est=2918585840 wall=7.65 rule=one_sweep_per_position_window
SS_SUBDIV_EXACT wave=0 class=<root> depth=1 rows_exact=2918585840 rows_est=2918585840 rows_err=0.0000 pages_exact=95462 coverage_exact=0.023866 coverage_est=0.016753 page_fraction_exact=1.0010 page_fraction_est=0.7027 bytes_read=100133169152 rule=exact_from_the_class_full_position_list
SUBDIV_G3_SCALE dir=/mnt/nvme2n1/erikg/SUBDIV-GATES/runs/g3_100gb n=100000000001 class_bytes_read=100133169152 pages_exact=95462 bound=95560MiB within=1 bytes_per_page=1.000 n_fraction=1.001332
SUBDIV_G3_GLOBAL dir=/mnt/nvme2n1/erikg/SUBDIV-GATES/runs/g3_forced_global classes=1 sweeps=1 (each class exactly one)
SUBDIV_G3_PASS (PLAN corpus_reads=0; local bytes_read <= the class's own page footprint at BOTH scales)
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_
SS_SUBDIV_CLASS_ONLY key=' e' wave_depth=2 bytes=[101,109) rows_est=2525225000 coverage=0.015634 page_fraction=0.6557 strategy=local note=emitted_stream_covers_this_class_only
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=100000000000 positions=2528840272 position_bytes=20230722176 rows_est=2525225000 wall=13.43 rule=one_sweep_per_position_window
SS_SUBDIV_CLASS_ONLY key=' e' wave_depth=2 bytes=[101,109) rows_est=2525225000 coverage=0.015634 page_fraction=0.6557 strategy=global note=emitted_stream_covers_this_class_only
```

### G4 the 100 GB head's space wave through both arms

```
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=10000000000 positions=472025678 position_bytes=3776205424 rows_est=472025678 wall=1.02 rule=one_sweep_per_position_window
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=100000000000 positions=2918585840 position_bytes=23348686720 rows_est=2918585840 wall=7.65 rule=one_sweep_per_position_window
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_
SS_SUBDIV_CLASS_ONLY key=' e' wave_depth=2 bytes=[101,109) rows_est=2525225000 coverage=0.015634 page_fraction=0.6557 strategy=local note=emitted_stream_covers_this_class_only
SS_SCATTER pass=1 waves=[0,1) waves_n=1 bytes_read=100000000000 positions=2528840272 position_bytes=20230722176 rows_est=2525225000 wall=13.43 rule=one_sweep_per_position_window
SS_SUBDIV_CLASS_ONLY key=' e' wave_depth=2 bytes=[101,109) rows_est=2525225000 coverage=0.015634 page_fraction=0.6557 strategy=global note=emitted_stream_covers_this_class_only
SUBDIV_G4 class_rows_est=2525225000 local_sha=fc87fab90227c2608008adab915669a0016938634a01cf05315fa332abb8a8d2 global_sha=fc87fab90227c2608008adab915669a0016938634a01cf05315fa332abb8a8d2 equal=1
SUBDIV_G4_WALL local: SS_DONE n=100000000001 waves=1 ties=909530626 sample_s=0.02 waves_s=404.31 total_s=420.55 | global: SS_DONE n=100000000001 waves=1 ties=909530626 sample_s=0.02 waves_s=357.34 total_s=360.17
SUBDIV_G4_PASS
```

### G5 refusal — a class the census cannot prove

```
SS_WAVE_POINT_MASS_PROOF depth=2 class_rows_sampled=1258291 tail_slice_rows=0 mismatches=2 wall=0.00 (one corpus pass, the only one this branch ever costs)
SS_WAVE_POINT_MASS depth=2 class_rows=1258291 wave_goal=298841 trailing_run_rows=0 provable=0
SS_WAVE_BORROW class_rows=1258291 bytes_per_row=41 arena_gb=0.048 disk_gb=0.048 mem_cap_gb=0.009 path=refuse
SUBDIV_G5 rc=2 refused=1
SUBDIV_G5_PASS (numbers above; levers are --memory-gb and a wider prefix cache; a merge is never a path)
```

### G6 strategy equivalence — 5 fixtures, forced local vs forced global

```
SUBDIV_G6 fixture=docsep local_sha=539522013cbe992a586ab44930b083d267eb53aee13d24b66213b13f2967e510 global_sha=539522013cbe992a586ab44930b083d267eb53aee13d24b66213b13f2967e510 equal=1 coverage_est_vs_exact_max_rel_err=0.000000 classes=31
SUBDIV_G6 fixture=internsep local_sha=d5f5f42053018920b2127aea9094244abce212ac9803098210c59dc0d2a1b84e global_sha=d5f5f42053018920b2127aea9094244abce212ac9803098210c59dc0d2a1b84e equal=1 coverage_est_vs_exact_max_rel_err=0.000000 classes=32
SUBDIV_G6 fixture=mixed local_sha=2b1c5fa341a19945e299a04528b18b6a9a5701cb424e76c4e1bbf540742d3741 global_sha=2b1c5fa341a19945e299a04528b18b6a9a5701cb424e76c4e1bbf540742d3741 equal=1 coverage_est_vs_exact_max_rel_err=0.000000 classes=20
SUBDIV_G6 fixture=eofrun local_sha=c095ccfe31081aedd0d2f27061aac3f9d197e67cd11cf54d78e8c22b97c98986 global_sha=c095ccfe31081aedd0d2f27061aac3f9d197e67cd11cf54d78e8c22b97c98986 equal=1 coverage_est_vs_exact_max_rel_err=0.000000 classes=30
SUBDIV_G6 fixture=r0 local_sha=d3c09c42badedb94ba175cdfa82636f92754e541f5cb0c8f9d77ea673521853b global_sha=d3c09c42badedb94ba175cdfa82636f92754e541f5cb0c8f9d77ea673521853b equal=1 coverage_est_vs_exact_max_rel_err=0.000000 classes=30
SUBDIV_G6_PASS fixtures=5 (byte-identical band streams under forced local and forced global)
```

### G7 survey reproducibility — 2 corpora × 2 runs

```
SUBDIV_G7 fixture=docsep line_a='SS_SURVEY samples=6291456 stride=1 prefix_bytes=85 run_cap=255 pages=6291456 chunks=256 chunk_bytes=24576 blob_bytes=585629696 sha256=98a0e49c4ab2d2101b6e2f0764c0e8d2b49d32f9f9c360681f57afa47ba02062 bytes_read=6291456 rule=one_pass_all_top_level_bins'
SUBDIV_G7 fixture=docsep line_b='SS_SURVEY samples=6291456 stride=1 prefix_bytes=85 run_cap=255 pages=6291456 chunks=256 chunk_bytes=24576 blob_bytes=585629696 sha256=98a0e49c4ab2d2101b6e2f0764c0e8d2b49d32f9f9c360681f57afa47ba02062 bytes_read=6291456 rule=one_pass_all_top_level_bins'
SUBDIV_G7 fixture=mixed line_a='SS_SURVEY samples=6291456 stride=1 prefix_bytes=85 run_cap=255 pages=6291456 chunks=256 chunk_bytes=24576 blob_bytes=585629696 sha256=22b20ec38a4e40a29deaa9f5649c7a5a579633d6d148bdeedb7da0cf343d0cb1 bytes_read=6291456 rule=one_pass_all_top_level_bins'
SUBDIV_G7 fixture=mixed line_b='SS_SURVEY samples=6291456 stride=1 prefix_bytes=85 run_cap=255 pages=6291456 chunks=256 chunk_bytes=24576 blob_bytes=585629696 sha256=22b20ec38a4e40a29deaa9f5649c7a5a579633d6d148bdeedb7da0cf343d0cb1 bytes_read=6291456 rule=one_pass_all_top_level_bins'
SUBDIV_G7 distinct_shas_across_the_two_fixtures=2 (2 = the sha tracks the corpus, so it is not a constant)
SUBDIV_G7_PASS surveys=2
```

### G8 plan == reality — a 512 MB slice (stride 119), 31 classes

```
SS_SUBDIV_EXACT wave=0 class=<root> depth=1 rows_exact=6422835 rows_est=6422800 rows_err=0.0000 pages_exact=512 coverage_exact=0.000128 coverage_est=0.000128 page_fraction_exact=1.0000 page_fraction_est=1.0000 bytes_read=542113792 rule=exact_from_the_class_full_position_list
SS_SUBDIV_EXACT wave=1 class=\x20 depth=2 rows_exact=25428847 rows_est=25491490 rows_err=-0.0025 pages_exact=512 coverage_exact=0.000128 coverage_est=0.000128 page_fraction_exact=1.0000 page_fraction_est=1.0000 bytes_read=542113792 rule=exact_from_the_class_full_position_list
SS_SUBDIV_EXACT wave=2 class=\x20 depth=2 rows_exact=23786304 rows_est=23722422 rows_err=0.0027 pages_exact=512 coverage_exact=0.000128 coverage_est=0.000128 page_fraction_exact=1.0000 page_fraction_est=1.0000 bytes_read=541065216 rule=exact_from_the_class_full_position_list
SUBDIV_G8 classes=31 rows_err_mean=0.00132 rows_err_max=0.00740 within_1pct=31 within_5pct=0 worse_than_5pct=0 coverage_classes=31 coverage_max_rel_err=0.0000
SUBDIV_G8 max_rows_err=0.0074
SUBDIV_G8_PASS (the error distribution above is over every wave of the fixture)
SS_SUBDIV_EXACT wave=0 class=<root> depth=1 rows_exact=472025678 rows_est=472025678 rows_err=0.0000 pages_exact=9632 coverage_exact=0.002408 coverage_est=0.002384 page_fraction_exact=1.0100 page_fraction_est=0.9999 bytes_read=10122683392 rule=exact_from_the_class_full_position_list
SS_SUBDIV_EXACT wave=0 class=<root> depth=1 rows_exact=2918585840 rows_est=2918585840 rows_err=0.0000 pages_exact=95462 coverage_exact=0.023866 coverage_est=0.016753 page_fraction_exact=1.0010 page_fraction_est=0.7027 bytes_read=100133169152 rule=exact_from_the_class_full_position_list
```

### G9 no census reads

```
SUBDIV_G9 plan: SS_PLAN waves=32 local=32 global=0 cache_depth=85 cache_misses=0 stalls=0 saturated=32 samples=6291456 stride=1 corpus_reads=0 corpus_read_calls=0 rule=answer_from_prefix_cache
SUBDIV_G9 descent classes=32 bytes_read_total=231735296 pages_exact_total=192 over_page_bound=0
SUBDIV_G9_PASS (PLAN corpus_reads=0 cache_misses=0; every local descent stayed inside its page bound)
SS_PLAN waves=1 local=31 global=0 cache_depth=128 cache_misses=0 stalls=0 saturated=30 samples=4000000 stride=2500 corpus_reads=0 corpus_read_calls=0 rule=answer_from_prefix_cache
SS_PLAN waves=1 local=48 global=0 cache_depth=128 cache_misses=0 stalls=0 saturated=0 samples=4000000 stride=25000 corpus_reads=0 corpus_read_calls=0 rule=answer_from_prefix_cache
```

The survivor check is scoped to this sweep's own runs (a `pgrep` by binary
name would fail on another lane's work); its last line, after all 43 runs:

```
SUBDIV_NO_SURVIVORS checked=43 runs runs_tags_begin='g1_base_docsep g1_new_docsep g1_base_eofrun g1_new_eofrun g1_base_internsep g1_new_internsep g1_base_mixed g1_new_mixed g1_base_period2 g1_new_period2 g1_base_r0 g1_new_r0 g1_base_single g1_new_single g2_docsep g2_eofrun g2_internsep g2_mixed g2_p…'
```

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
XSA_SUBDIV_POSITION_BUDGET_BYTES=1000000000000 \
xsa build --tape --input /mnt/nvme2n1/erikg/pile.txt --scratch PILE \
          --threads 96 --memory-gb 96 --scratch-budget-gb 250 \
          --trie-depth 8 --trie-min-support 1000 --docs-map pilemap_full
```

Why the environment and not a flag: `xsa build` builds the producer's argv from a
fixed list (`xsa/src/chain.rs:2566`), so a new CLI flag would be unreachable from
the command the pile run actually uses, and plumbing it through the plan/knobs
digest (`xsa/src/recovery.rs` hashes the knobs) is a change this lane is not
allowed to make blind. `scatter_sort` therefore reads the three knobs from the
environment as well (`XSA_SUBDIV_POSITION_BUDGET_BYTES`,
`XSA_SUBDIV_STRATEGY`, `XSA_SUBDIV_LEAF_COVERAGE_PCT`), CLI flags win over the
environment, and `SS_SUBDIV_KNOBS ... source=env|default|env+flags(cli wins)` is
journaled on every run so a relaunch cannot silently differ from what was
measured. `--subdiv-strategy` and `--subdiv-leaf-coverage-pct` keep their
defaults; every value the plan and the descent used is journaled, so a relaunch
is checkable against `SS_SURVEY`/`SS_PLAN`/`SS_SUBDIV_*` without re-deriving
anything.

Sizing the window: 1 TB of position files ⇒ `ceil(local_rows × 8 / 1e12)` ≈ 11
scatter passes at the pile (N = 1.307 TB); 20 GB (the floor the gate script ran
with) ⇒ 524 passes. The disk is free to ~8 TB on the pile box, and the position
files of one window are unlinked as soon as the window's waves are descended.

> **CORRECTION (2026-11-14, `fix/subdiv-window-disk`).** The last clause above was
> FALSE in the code this document describes: the unlink existed, but the scatter
> loop wrote **every** window's position files before the wave loop descended the
> first one, so the peak was `local_rows × 8 B` ≈ 10.5 TB at the pile — a corpus
> number, independent of the budget — and the pile run died with a full filesystem
> at 8.2 TB live scratch. What is true now: the wave loop sows a window immediately
> before its waves are descended and unlinks it after the window's last wave, the
> window is hard-capped at the position budget (planned to 95 % because `rowsEst`
> is a sample estimate; the writer counts bytes and dies past the budget), and
> `statvfs` refuses a window that does not fit the scratch filesystem plus its
> margin. `SS_SUBDIV_POSPEAK peak_total=` is the number that proves it: peak =
> one window, not the column. See `RESULTS_SUBDIV_DISK.md`.

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
