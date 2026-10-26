# The tape machine, prototype: exact SA, verified, 8:25 for 10 GB

## What was built

`tools/binbwt/scatter_sort.cpp` — the tape design, in four stages:

1. **SAMPLE**: jittered-stride 16-byte keys → sorted → quantile BAND BOUNDARIES.
   A sample suffices: bands only need balancing, and every row is routed by a key
   *comparison*, so sampling error can unbalance a band but can never misorder a
   row. (Pseudo-random would cost random I/O for nothing; pure stride can alias
   with document/markup structure, hence the jitter.)
2. **SCATTER**: ONE sequential pass over the tape. Each row becomes
   `(16-byte big-endian key, position)` and is appended to its band's file.
   The text is read once and **never again during sorting**.
3. **SORT**: per band, in RAM, bands in parallel. Keys compare as two big-endian
   u64s; only rows with EQUAL 16-byte keys fall through to a real suffix
   comparison.
4. **EMIT**: bands are key ranges, so emitting them in boundary order IS suffix
   order — a plain concatenation.

`tools/binbwt/sa_check.cpp` — the acceptance gate. Verifies the DEFINING
properties directly (stronger than comparing against another machine's output):
permutation (every position exactly once) and strict suffix order.

## The convention, established empirically

The product path is **dollar=1** (plain suffix order; a shorter suffix that is a
prefix of another is smaller). Proven by emitting the bin machine's own full SA
on a 4 MB real-text slice (`--sa-sample-k 1`) and checking it both ways:
dollar=1 passes all 4,000,001 ranks; cyclic fails at rank 62,626.

## Results

| scale | rows | sample | scatter | sort+emit | **total** | verify | verdict |
|---|---|---|---|---|---|---|---|
| 4 MB | 4,000,001 | 0.05 s | 0.06 s | 0.17 s | 0.32 s | — | **byte-identical** to the bin machine's SA (payload cmp) |
| 1.07 GB | 1,073,741,825 | 0.39 s | 1.73 s | 29.6 s | **43.4 s** | 69 s | **SA_CHECK_PASS** |
| 10 GB | 10,000,017,258 | 3.06 s | 41.96 s | 379.8 s | **8:25** | 28:21 | **SA_CHECK_PASS** |

10 GB peak RSS 27 GB; SA stream 80 GB; 241 bands, 12 sorted concurrently.

**For scale:** the bin machine's 10 GB producer ran 25:33 (1,533 s) and also
emitted the rle-BWT and rungs. The tape prototype is 3.0× faster on the same
corpus while doing LESS so far — the comparison is not yet apples-to-apples.

## Where the time goes, measured

`ties = 5,194,940,776` of 10,000,017,258 rows (**52 %**) share their 16-byte key
with at least one other row, and each pays a real suffix comparison. That is why
sort+emit is 380 s of the 505 s total. **Scatter — the entire text pass — is
42 s.** The machine is comparison-poor and I/O-rich, which is the opposite of the
bin machine and is exactly the trade we wanted.

## The estimate I got wrong

Extrapolating the measured 50 s/GB to the pile: 1.31 TB ⇒ **~18 hours for the SA
alone**, not the ~3 h I predicted. That is a 6× miss, recorded here rather than
quietly revised. It is still ahead of the bin machine's ~52 h *estimate* (which
has never completed a 100 GB rung), and the remaining cost sits in a tractable
place.

## The optimization path (measured bottleneck, not speculation)

1. **32-byte keys**: we are I/O-rich (42 s) and comparison-poor (380 s), so
   trading bytes for a smaller tie set is expected to win.
2. **MSD radix on key bytes with early exit** instead of comparison sort: ~28
   comparisons/row become a handful of byte-indexed passes.
3. **Resolve ties by key EXTENSION, not LCE**: for an equal-key run, gather the
   next 16 bytes and sort on that; recurse. Never compare long runs of text.
4. **Shrink the record** (24 B prototype → ~12 B at pile: 4-byte key remnant +
   6-byte position) for memory/band sizing, not for wall.

None of these touch the correctness argument, and `sa_check` re-verifies after
each change.

## What is NOT done yet

- **Product emission.** The tape machine emits the SA; it does not yet emit the
  rle-BWT + rungs that `xsa_finish` consumes, so no container has been built
  from it and no byte-identity against a banked container has been shown.
- **The tie-tail at pile scale.** The survey (kgram_survey) measured the
  10 GB tail as 1.24 % uniform runs + 0.038 % genuine repeats at a 12-byte key;
  the pile's numbers are still computing.

## Optimization attempts that REGRESSED (all verified correct, all slower)

Recorded because a negative with a mechanism is worth more than a silent revert.
Every variant below produced a `SA_CHECK_PASS` at 1.07 GB and was still slower
than the simple comparison sort (29.6 s), so none was kept.

| variant | 1 GB sort+emit | mechanism of the regression |
|---|---|---|
| baseline: 16-byte key + `khi` integer tie check + `std::sort` | **29.6 s** | — |
| LSD radix on the 8-byte prefix, index array permuted | 57.7 s | the radix passes read `rows[idx[i]]`, i.e. RANDOM access into 24-byte records, 4 passes x 2 reads — it converts a mostly-sequential comparison sort into a random-access one |
| LSD radix, 4 x 16-bit passes (2 passes was a bug: only 32 of 64 key bits) | — | fixed the bug; the access pattern conclusion above stands |
| MSD distribute on the top 16 bits, then per-bucket sort | 50.5 s | needs a second full row buffer; allocating/freeing 1 GB per band turned into a page-fault storm (sys 6m27s of a 63s run) |
| Row shrunk to 16 B (drop `khi`) | 53.2 s | equal-8-byte-key rows now ALL fall through to a real suffix comparison (~60% of rows vs 52% at 16 bytes), and each `suf_cmp` costs more than an integer `khi` compare |

Also observed: identical code measured 43.4 s and 57.2 s in two runs — the
difference is page-cache state of the corpus. Single-run timings on this box
carry ~30% variance; only same-session A/B is trustworthy.

**Where that leaves the sort**: it is memory-traffic bound, and the lever that is
left is a properly packed layout (12-16 B rows with the key remainder, positions
reused buffers, no per-band allocation), not a different sorting algorithm.

## THE PRODUCT AT 10 GB: byte-identical to the bank

With the remap bug fixed and the symbol stream buffered, the tape machine produces
the banked container exactly:

| | value |
|---|---|
| runs (r) | **3,474,891,368** (exactly the bank's; 1,519 short before the fix) |
| chi | **2,676,929,430** (the finish's in-process assert passed) |
| container sha256 | **e99fc0b35e7fadb030934f7c75f0b62d4d0631a626213f5173f2eca599f5f688** |
| bank sha256 | same |
| SA verification | SA_CHECK_PASS, all 10,000,017,258 ranks, in PRODUCT (remapped) order |

Stages: scatter+sort 848.6 s (sample 1.51, scatter 37.21, sort+emit 747.71);
emit 1161.15 s (word 296.09, publish 852.28, read 12.13); finish fused and
overlapped.

### HONEST ACCOUNTING: it is not faster yet

Product wall ~33.5 min vs the bin machine's ~25.5 min producer at the same 10 GB.
The tape machine is ~30% SLOWER at this scale. Its value so far is (a) exactness
proven at 10 billion ranks in product order, and (b) a cost structure with no
random access at all. Pile projection from the measured rates as-is: ~31 h sort +
~42 h emit ~= 3 days - comparable to the bin machine's ~4-day estimate, not yet a
win.

Three measured levers, all engineering rather than architecture:
1. the remapped comparator costs 2x (sort+emit 380 -> 748 s when the remap branch
   went in); a 64 KB remapped lookaside or remapping a key block once per
   comparison recovers most of it;
2. `publish` is single-threaded (852 s of the 1161); word-building is already
   chunked and parallelizable;
3. the row is 24 B; the pile plan is 10-12 B.

### A GATE FINDING THAT MATTERS

The remap bug produced the EXACT SAME chi at 1 GB (303,812,551) and at 10 GB
(2,676,929,430) while emitting the WRONG permutation (r short by 25 and 1,519
respectively). **chi alone would have certified a wrong build.** Only container
byte-identity caught it. This is the concrete case for the arbiter this project
enforced all along, and it belongs in the paper.

### Bug ledger for this stage (all found by running, not by review)

- one `text.at()` per row = the LOCKED windowed corpus adapter, 1e10 times: 34
  cores for 50 minutes publishing nothing. Fixed by carrying the BWT symbol in
  the row (free at scatter time).
- the sink's flow control (`v2_flow_wait`) REQUIRES a live consumer: finish-after-
  emit deadlocked; the product path runs them fused. Fixed by starting the finish
  first.
- one `write()` per row for the symbol stream: sort+emit 380 -> 1752 s. Buffered.
- keys built in the RAW alphabet instead of the product's remapped one (above).
- `sa_check` had no `--remap`, so it validated the wrong order. Added.

## THE 2n PIPELINE PRODUCES THE BANK (10 GB, byte-identical)

Scratch capped at 2n by construction (waves sized from the cap, each wave's rows
deleted before the next is written):

| | value |
|---|---|
| scratch cap | 21,500 MB (2.0n) |
| peak scratch measured | 22,070 MB (2.06n; 1.95n after the 95% fill fix) |
| SA verification | SA_CHECK_PASS, all 10,000,017,258 ranks, PRODUCT order |
| order wall | 1,365 s (20 waves) |
| emit + finish wall | 1,647 s (publish 1,024 s of the emit) |
| chi | 2,676,929,430 exact |
| runs | 3,474,891,368 exact |
| container | e99fc0b35e7fadb030934f7c75f0b62d4d0631a626213f5173f2eca599f5f688 |
| bank | same |

Total ~50 min at 10 GB under a 21.5 GB scratch cap, against the bin machine's
~25.5 min with unbounded windows.

### The scaling law this establishes

    waves   = row_bytes / 2
    I/O     ≈ 2.5 x row_bytes x n
    scratch = 2n, flat in corpus size

At pile (1.31 TB) with 32-byte rows: ~16 waves, ~105 TB of sequential I/O, peak
scratch 2.6 TB on a 14 TB disc. Every number is computable BEFORE launch.

### Measured trade-off: the row size dial (1 GB @ 2n)

| row | key | waves | ties | wall |
|---|---|---|---|---|
| 16 B | 8 B | 11 | 88% | 152 s |
| 24 B | 16 B | 19 | 37% | 113 s |
| 32 B | 24 B | 20 | 13% | 88 s |

Smaller rows buy sequential I/O and multiply RANDOM text reads. At pile a tie read
against 1.31 TB is a disc random read, so 13% vs 88% ties is ~4e11 vs ~1e12 random
reads. The 32-byte row is kept for the pile for that reason, not for wall time at
10 GB.

### Null results recorded

- per-wave thread/buffer churn was NOT 34% of the wall: a persistent pool changed
  88 s -> 90 s. The sys time is band-file I/O.
- the write-read-write append copy was not the bottleneck either; direct pwrite at
  computed offsets left the wall unchanged.

## PIPELINE SHAPE: fusing the emit with the finish costs 3.8x (contention, not work)

Measured at 10 GB, same tree, same SA, same box:

| configuration | emit wall | publish | notes |
|---|---|---|---|
| emit ALONE (window unbounded, no consumer) | **340 s** | 103 s (10 ns/row) | true cost of the emit |
| emit FUSED with the finish (96 + 96 threads) | **1,292 s** | 1,051 s | 3.8x |

The emit does not do more work when fused - the two processes contend for memory
bandwidth. 1,050 - 103 = ~950 s of pure contention.

This explains the apparently superlinear publish cost: 13 ns/row at 200 MB (where
both stages are small), 19 ns/row at 1 GB solo, 58 ns/row at 1 GB fused, 105 ns/row
at 10 GB fused. **publish itself is ~10-19 ns/row.**

CONSEQUENCE for the pipeline design: for the tape machine, fusion is not obviously
right. The bin machine's finish was the long pole, so fusing helped (31 min vs
54 min at 10 GB). Here the emit is 340 s and the finish is the long pole, so the
right shape is either strictly sequential (340 + finish) or a THREAD-PARTITIONED
concurrency (emit 24 / finish 72) rather than 96 + 96.

## WAVE IMBALANCE BUG (found by the per-wave timers)

`SS_WAVE` showed wave 2 at 10 GB: wall 352 s with a single band's sort at 341 s.
Cause: for a TWO-LEVEL wave the band quantiles were computed over the whole first
byte's sample instead of the wave's two-byte range, so bands were wildly unbalanced.
Fixed. NOTE: re-measurements taken while another stage was running are contaminated -
the 1 GB numbers in this session vary by 25% depending on what else is on the box,
so single-run comparisons are only valid on an idle machine.

## PIPELINE SHAPE, settled by measurement: FUSED wins

10 GB, same tree, same SA, clean box both times, both byte-identical:

| shape | emit | finish | total |
|---|---|---|---|
| sequential (emit, then finish) | 374 s | 1,637 s | **2,011 s** |
| fused (both concurrently, 96+96) | (1,292 s contended) | overlapping | **1,661 s** |

Fusion wins by 17% - not because contention is cheap (it costs the emit 3.8x) but
because THE EMIT FITS INSIDE THE FINISH: 374 s of emit against 1,637 s of finish.
Overlapping it is nearly free.

## THEREFORE THE PILE BOTTLENECK IS THE FINISH, NOT THE PRODUCER

| stage | 10 GB | pile (x131) |
|---|---|---|
| order (scatter+sort, 2n cap) | 804 s | ~29 h |
| emit | 374 s | ~13 h (overlapped) |
| FINISH | 1,637 s | ~59 h |

164 s/GB of finish is the same serial fold spine the earlier charters measured
(~100 s/GB at the 1 GB fixture, worse here on a 3.47e9-run stream). It is not a
tape-machine artifact: it is the chi construction.

PILE ARITHMETIC: ~29 h order + ~59 h finish ~= 3.7 days, peak scratch 2.6 TB.

The tape machine cut the producer from ~52 h (bin machine, unverified at 100 GB) to
~29 h with a 2n scratch guarantee and exactness proven at 10e9 ranks. The next
target is the finish's serial fold, or fewer/longer runs so the fold has less to do.

## KASAI COLUMN AT 10 GB: a 2.7x LOSS (negative result, kept off)

The finish's sweep is 1,312 s of which lce_wall = 1,302 s (and `lce_threads=96`,
so the LCE queries are ALREADY parallel; they are the critical path regardless).
The Kasai arm removes those queries by reading a prebuilt LCP column. Building that
column from the 10 GB tape SA cost:

    invert_s=3294.9 (benchmarked: ISA inversion is a random scatter of 1e10 entries,
                     ~330 ns each), dump_s=300.8, walk_s=263.8 (the ratchet itself,
                     2.000n comparisons - cheap as designed)
    total 4,476 s, column 13.39 GB

Against 1,302 s of LCE saved, that is a 3.4x cost for a 1.3x saving: a 2.7x LOSS.
The Kasai charter measured a wash at the 1 GB fixture; at 10x it clearly loses
because the inversion scales with the random-scatter cost while the LCE it replaces
is a cached walk. **The arm stays off.**

Also found on the way: the tape SA's private header ('SXLC') made kasai_column
refuse the stream, which SILENTLY left the arm off. Fixed (SXSA anchor format).

## WHERE THE FINISH'S TIME GOES (10 GB, 1,637 s)

    sweep            1,312 s   (lce 1,302 | repair 565 | advance_witness 612 - overlapping)
    m5-ef              163 s
    m1-runs            156 s
    publish              1 s

The finish is LCE-bound on a 96-thread critical path. The ideas that remain are
structural, not tuning: capture the LCP of each adjacent pair WHILE THE PRODUCER
SORTS (the comparisons already happen there - 83-93% of build CPU is leaf LCE, and
adjacency information is produced as a side effect), rather than re-deriving the
column afterwards through an inversion.

## THE KASAI ARM'S REAL REQUIREMENT (and why the ISA is not the answer)

`xsa_finish`'s Kasai arm calls `locateIsa.at(head)` and `.at(tail)` - it needs the
RANK of each run's head and tail POSITION. Its `--sa-locate` format is a full ISA
(32-byte header + n x width), which is:

    10 GB corpus -> 80 GB file      pile -> 10.5 TB      <- breaks the 2n law

So a full ISA is not shippable at pile. But three facts make it unnecessary:

 1. only run ENDPOINTS are ever queried: 2r lookups, not n;
 2. ranks are MONOTONIC in run order (runs are published in rank order), so the
    finish can PREDICT every run's head rank by prefix-summing run lengths;
 3. the sink already knows each run's TRUE head/tail rank as it publishes (the row
    index at run open and at close), so the deviations are cheap to record.

The deviations are the "holes" the code already documents ("a len-2 run whose ranks
are {4576075, 4576077}"), left by the class/dollar repair - rare, not systematic.

=> THE 2n-COMPATIBLE SUPPLY: finish derives ranks by prefix sum; sink emits a tiny
exception list (run, true head rank, true tail rank). No inversion, no 10.5 TB.

Status: designed and started; the sink-side edit was reverted rather than left half
-wired (it landed in the wrong class - binbwt_slice.h has more than one flushRun and
one finish()). The tree is green at 41e4ce1 with everything else landed.

## RUN RANKS BY PREFIX SUM: where it stands, and the one thing it gets wrong

DONE and measured:
 - BinBandSink carries a rank counter and prefix-sums run lengths, emitting ONLY the
   deviations it knows for certain: <prefix>.runexc, 'SXRE' + count + triples
   (run index, true head rank, true tail rank). 65,535 exceptions of 57,279,189 runs
   at 150 MB (0.11%), 1.57 MB of file - not the 80 GB / 10.5 TB ISA.
 - xsa_finish gains --run-ranks and a RunRankExc provider; the Kasai arm accepts
   --run-ranks INSTEAD of --sa-locate, and the lcp-column escape list is deduped
   (a band's own merge and the seam can escape the same rank, and the column
   requires STRICT ordering).

BLOCKER, and it is semantic not mechanical: the arm now turns ON and then fails with
    FATAL: KASAI: an interior run collapsed to one rank
The finish's runs are POST-REPAIR (RepairStream merges/splits runs downstream of the
sink), so a finish run index and length are NOT the sink's. Prefix-sum + exceptions
keyed by the SINK's numbering do not line up with the finish's.

That is exactly why the original design reached for an ISA: it answers by POSITION,
which survives any renumbering. To keep the 2n law the exceptions must therefore be
keyed by something the finish still recognises after repair - the run's HEAD
POSITION, not its index. The sink knows the head position of every run it deviates
on; the finish can look up by position.

Status: sink side lands and is measured; finish side is additive and OFF unless
--run-ranks is passed (the ISA arm is untouched), so the product path is unaffected.

## THE RANKS NEED THE INVERSE: three designs, one blocker (negative result)

Attempted, in order, to supply the Kasai arm's run ranks WITHOUT the 10.5 TB ISA:

 1. exception list keyed by RUN INDEX   -> the repair renumbers runs.
 2. exception list keyed by HEAD POSITION -> the repair changes run LENGTHS, so a
    cursor drifts wherever it does (measured: prefix-sum rank outside [0,n]).
 3. prefix sum over the finish's OWN post-repair stream -> measured closure counters
    rank_holes=262459 disorder=261744 noncontiguous_runs=67321. The stream is NOT in
    rank order, so no cursor over it can equal a rank.

CONCLUSION: for this arm a POSITION -> RANK inverse is load-bearing, not an artifact
of the implementation. The 10.5 TB ISA does not fit the 2n law, so the honest options
for the pile are:

 (a) run the arm only where an ISA fits (up to ~100 GB: 0.8 TB at 8 B), and run the
     old LCE arm at pile (~59 h of finish);
 (b) store the inverse SAMPLED (every k-th position) and resolve a query by walking
     the move structure (phi) from the nearest anchor - the r-index query path,
     which the container already has as member 11 - trading 10.5 TB for ~n/k * 8 B
     plus bounded walk steps;
 (c) make the repair stream rank-ordered, which would make design 3 valid and needs
     no inverse at all - the deepest change and the only one that removes the cost
     rather than amortising it.

The arm now REFUSES to publish when the closure counters are non-zero.

## SAMPLED INVERSE: built, verified, and still not sufficient (4th negative result)

Built the whole sampled-inverse path:
 - producer: --isa-sample-out/-k writes ISA[p] = rank for every k-th POSITION, inside
   the band worker's output loop (rank = band_off + index). Anchors VERIFIED: 25/25
   by seeking SA[anchor] and comparing to p (tools check in the session log).
 - finish: --isa-sample loads a SampIsa, and the fold RESYNCS its rank cursor at every
   anchored head position.

Result at 150 MB with k=64 (18.7 MB of anchors, 0.125n):
    rank_holes=175918  disorder=175550  noncontiguous_runs=37672

WHY IT STILL FAILS: resynchronising at anchors only helps if the cursor is right
BETWEEN them, and that assumes consecutive stream runs are consecutive in rank. They
are not (disorder 175,550 runs). Answering rank(p) for an unanchored p therefore needs
a POSITION -> POSITION step in rank space (psi = inverse phi), not just an anchor.

So the four attempts localise the requirement precisely:

   run index keying        -> repair renumbers
   head-position keying    -> repair changes LENGTHS (cursor drifts)
   prefix sum over stream  -> stream is not rank-ordered
   sampled ISA + resync    -> stream disorder is commoner than the anchor spacing

Remaining sound options:
 (a) full ISA where it fits (about 0.8 TB at 100 GB), LCE arm at pile;
 (b) sampled ISA + PSI (inverse phi, which bit6/phi_inverse_heads.cpp can build) so an
     unanchored query walks forward in position space - bounded steps, r entries;
 (c) rank-ordered repair stream, which makes approach 3 valid with no inverse at all.

The arm keeps its refusal guard, so none of this can publish a wrong container.

# THE RANK SUPPLY FOR THE KASAI ARM — the design, stated once, unambiguously

## What is needed
`xsa_finish`'s Kasai arm asks, for every run, `rank(head position)` and
`rank(tail position)`. That is the INVERSE of the suffix array at 2r points.

## Why the obvious supplies fail (four measured attempts, do not repeat them)
| supply | why it fails |
|---|---|
| full ISA | 80 GB at 10 GB, **10.5 TB at pile** - breaks the 2n law |
| exceptions keyed by run index | the repair RENUMBERS runs |
| exceptions keyed by head position | the repair changes run LENGTHS -> cursor drifts |
| prefix sum over the finish's own stream | the stream is NOT rank-ordered (disorder 175,550 at 150 MB) |
| sampled ISA + RESYNC ONLY | resync fixes the anchors; between them the cursor still assumes rank order, which is false |

**The last row is the confusion to avoid: anchors alone are not the design. The WALK
is the design.** An anchor only fixes a starting point; reaching an unanchored position
requires stepping in rank space.

## The design that is sound: anchors + PSI walk
Two facts:
 1. `ISA[p+1] = psi(ISA[p])`   where psi is the forward BWT step (inverse of LF/phi).
 2. anchors: `ISA[a]` stored for `a = 0, k, 2k, ...`   (n/k entries, 10 GB at pile k=1024)

So `rank(p)`: take `a = p - (p mod k)`, read `ISA[a]`, then apply psi `(p - a)` times.
Across the fold the head positions are NEARLY monotone, so the (position, rank) pair is
CARRIED from run to run: total stepping is O(n) for the whole fold, not 2r x k, and the
anchors are used only to re-enter after a backwards jump.

## What psi needs
`psi(i) = select_BWT(c, i - C[c] + 1)` for the c with `C[c] <= i < C[c+1]` - i.e. SELECT
over the run-length BWT. `phi_inverse_heads.cpp` emits `head[j] =
inverse_phi(tail[(j+R-1)%R])`, a raw u64 inverse-phi over the run table, which is the
materialised form of the same step; either supply works, and psi as a table is
`r` entries (4.5e11 x 8 B = 3.6 TB at pile - so prefer the SELECT form, which is O(r)).

## Status
 - anchors: written by the producer, VERIFIED (25/25 by seeking SA[anchor]).
 - SampIsa reader and the resync are in the finish and correct as far as they go.
 - THE WALK IS NOT IMPLEMENTED. That is the remaining piece, plus SELECT over the RLE BWT.
 - The arm REFUSES to publish unless holes/disorder/noncontig are all zero, so no
   unsound supply can bank a container.

## MEASURED: how wrong the derived ranks actually are (fixture-scale ground truth)

XSA_FINISH_ISA_DEBUG=<SA stream> builds the TRUE inverse (fixture scale only) and compares
every run's derived head rank:

    150 MB, anchors k=64:   deviating_runs = 13,853 of 57,279,187  (0.024%)
                            max_dev = 11,132    mean_dev = 3.80
    closure:                rank_holes=435  disorder=413   (was 175,918 without anchors)

Consequences, and they decide the implementation:
 - anchors alone fix 99.976% of runs, so the STRUCTURE is right and only the tail needs
   help;
 - k alone cannot fix it: the drift reaches 11,132 ranks, so no affordable k covers it
   (k=8 would cost 1.0n and STILL miss the tail);
 - therefore the PSI WALK is needed, but only as a CARRIED pair: advance
   (position, rank) from run to run and re-enter from an anchor after a backwards jump.
   With a 0.024% excursion rate the re-entries are rare, so the total stepping is O(n),
   not 2r x k.

This diagnostic is the acceptance test: deviating_runs must be 0 with the walk in place.

# THE KASAI ARM IS EXACT (2026-10-26): three defects found, all gates green

Worked the verification pyramid DOWN to a 454 KB fixture first, and every layer
after that was a gate, not a hypothesis. Three defects, each measured:

## 1. THE PHASE-1 RACE (the root of all four negative results above)
The rank cursor ran redundantly in EVERY pool worker over the same shared
arrays. A worker that woke late read an already-advanced `rowCursor`, mixed
foreign values into `headRank`/`tailRank`, and double-advanced the stream for
the next band. Measured at 150 MB with the race present: deviating_runs
21,626,880 of 57,279,187, rank_holes 35,653,440, and DIFFERENT closure
counters on every run of the SAME deterministic pieces (emit verified
byte-identical, rlebwt sha 2d9f898e...). **All four "the stream is not
rank-ordered" negative results recorded above were measured through this
race.** Phase 1 now runs on worker 0 alone behind the existing arrival
barrier.

## 2. THE COLUMN CONVENTION (the arm's systemic answer defect)
The finish read `col[r-1]` for the top LCP and `min(col[a..c-1])` for the
interior, assuming `col[r] = LCE(SA[r], SA[r+1])`. scatter_sort's
`--lcp-out` emits the STANDARD left-neighbour array `LCP[r] = lce(SA[r-1],
SA[r])` - verified 300/300 at the fixture against brute-force cyclic LCE.
The off-by-one shifted every column answer one run late: 140,543 of 192,138
answers differed from the walk arm at the 2 MB fixture (chi 172,940 vs
145,686). This defect was present in EVERY kasai-arm chi ever measured
(150 MB: 49,091,769 vs the v1 arm's 44,034,233). Reads are now
`col[r]` and `min(col[a+1..c])`.

## 3. THE SEAM CLASS TRUE ORDER (the real rank defect, 0.014% of runs)
With the race fixed, the 150 MB cursor is exact for 57,271,333 of 57,279,187
runs; the residual 7,854 runs live inside the two seam-class spans, and the
mechanism is now exact: the repair emits each class in cyclic-content order,
but the TRUE suffix array places the class SEED (the chain seed at position
n-d - the wrap suffix that ends at the text end; the SA's sentinel convention
for the cyclic boundary) FIRST and the remaining occurrences in that same
content order. Verified 101/101 at the 454 KB fixture by brute force and at
150 MB/1 GB by the position anchors at every anchored class occurrence
(25,469 + 183,772 checks, zero mismatches). The splice stamps each class
run's true head/tail rank (`NRun::ht/tt`); phase 1 takes the stamp over the
cursor. The lo/hi span placement was CORRECT all along.

## THE PSI-WALK NEGATIVE RESULT (mechanism, as the task's rules requested)
The prescribed anchors + sparse-select supply was built and measured: at 150 MB
53.76% of consecutive run heads jump BACKWARDS in position (so a carried pair
along the stream costs 7.8e6 x n of forward stepping), and - decisively - the
SA is not LF-consistent: 39 rows at the 454 KB fixture where
`LF(row) != isa[SA[row]-1]`, all in the separator class, caused by the same
wrap-suffix sentinel convention. Anchor-to-anchor psi chains therefore
disagree with the sampled ISA and REFUSE (the chain check fired, as designed).
Exact ranks on this corpus family cannot come from select-based psi; the
cursor + class true-order stamps + anchor verification is exact, O(1) per run,
needs no pre-pass and no run table, and the anchors certify it at any scale.

## THE GATES (all with the packaged runtime-mirror build)

- 454 KB fixture: kasai chi 145,686 == v1 chi 145,686; containers
  byte-identical (e91f9d9c...5aa3); deviating_runs=0.
- 150 MB (`/dev/shm/t150m.txt`, rxt2/col, is2.sa/is2.isamp):
  `XSA_FINISH_RANK_DEV deviating_runs=0` over all 57,279,187 runs vs the true
  ISA; kasai chi 44,034,233 == v1 chi 44,034,233; containers byte-identical
  (`24688d22...ae05`); `XSA_FINISH_KASAI_ANCHORS checks=1154503 mismatches=0`;
  `XSA_FINISH_SEAM_ANCHORS checks=25454+15 mismatches=0`.
- 1 GB (`ss-1g.txt`, col1g triple): **container sha256
  `31ce2e4f4b5990d5a9c79393e4092777992a326accdc2a7bd1b726fd58c03402`, exactly
  the banked value, with the Kasai arm ON**; chi 303,812,551 asserted
  in-process; deviating_runs=0 over all 394,663,026 runs (8.6 GB true ISA);
  7,938,716 endpoint anchor checks + 183,772 class-occurrence checks, zero
  mismatches. Wall: emit 48 s, finish ~19 min (includes the 8.6 GB debug ISA
  build; without it ~14 min).

## THE CLOSURE COUNTERS: what zero means, and why they are not zero
`rank_holes`/`disorder` count the runs where the STREAM order breaks against
the TRUE order. With exact ranks they are 6/2/0 at 150 MB and 12/4/0 at 1 GB -
the honest count of the seed-displacement breaks inside the seam classes
(3-4 per class). These breaks are BANKED: the v1 arm's answers (and the
container shas, including the banked 31ce... and the 10 GB product) are
computed on the stream in exactly this order, and each break is covered by
the walk fallback, which reproduces v1's answer by construction (proven by
the byte-identical containers). Making the counters zero would mean
re-ordering the seam classes in the stream - which changes the v1 arm's own
answers and therefore the container sha: the "closure all-zero" criterion and
the banked-parity criterion are mutually exclusive. Parity is the product
gate and it is green at both scales.

# DECISION (operator): adopt SEED-FIRST. The bank parity is superseded.

The two conventions differ only at the wrap-sentinel seam:
  seed-first (finite-suffix order): closure 0/0/0, fallbacks 0 at every scale,
      chi 145685 / 44034231 / 303812548, 1 GB runs 394663025
  bank (cyclic order):              closure 6/2, 12/4, walk fallbacks, chi +1..+3,
      1 GB runs 394663026
Mechanism: the finite-SA BWT and the bank's cyclic BWT differ at 26 rows on the
454 KB fixture from rank 5615; the displaced seed sits at finite rank 5615 but the
cyclic rebuild places it at row 5653, so "fixing" the order rotates intervening bytes.

WHY THE OPERATOR CHOSE SEED-FIRST: the displacement is a wrap-sentinel artifact, not a
property of the index. Seed-first is rank-ordered BY CONSTRUCTION, which makes the
closure counters meaningful (zero), removes the walk fallback entirely, and keeps the
Kasai arm's column as the only rank input. A machine that needs a fallback to paper over
its own stream order is "weird"; this one is not.

CONSEQUENCE: the old banked shas (e91f9d9c, 24688d22, e99fc0b3) are SUPERSEDED. They
certified the cyclic convention. A sha can no longer be the arbiter.

## THE NEW ARBITER (stronger than a sha)
 1. STREAM == BWT(SA). Expand the emitted band stream (chars + lens) to the full BWT
    string and compare it, row by row, with T[SA[r]-1] computed from the VERIFIED SA
    (sa_check already proves the SA exact in the product alphabet over all n ranks).
    Zero mismatches over all n rows is a direct proof of the stream, not a fingerprint.
 2. CHI independently re-derived at fixture scale from the true SA (brute force), and
    the T3 claims checker at larger scales.
 3. Closure counters must be EXACTLY ZERO (holes/disorder/noncontig) and
    walk_fallbacks == 0, with the refusal guard enforced and no debug escape.

## ADVANCE_WITNESS: measured at cycle level, and the heap WON (142 vs 183 cy/run)

In-sweep rdtsc split of advance() at 150 MB (57,279,186 runs, every one a character change):

    heap threshold_reset : total 220.6   thresh 142.3   other  78.2  cycles/run
    AVX2 256-wide scan   : total 245.7   thresh 183.4   other  62.3  cycles/run
    unimstrumented wall  : 6.2 s either way (108 ns/run, ~270 cycles at 2.5 GHz)

So the active-set heap is ~19% cheaper in the threshold path and the AVX2 sweep is the
slower of the two (2 KB across 32 cache lines + movemasks per run to find 1.47 entries).
The "wall neutral" note in the heap commit was wrong at the precision available; the
cycle counters settle it. Heap retained.

REMAINING SPLIT: threshold ~65%, the emit/state path ~35% (per-run NRun read, two
witness() calls, the std::function emit into WitBuckets::add). Floor for an O(R) fold at
~20-30 cy/run would be ~4-5 h at pile against today's ~16 h, so advance_witness is worth
at most a 3x and it is NOT the whale.

## THE FINISH'S PILE BUDGET, per term (150 MB measured -> 1.31 TB)

    sparse-row LCE (1 per 1024 runs, ~450 us each)  24.4 s -> ~59 h   <-- THE WHALE
    repair                                          17.4 s -> ~44 h
    advance_witness                                  6.2 s -> ~16 h
    total                                           ~48 s -> ~117 h

The sparse-row LCE term is answerable from the column we already built: its two ends are
run endpoints whose ranks the cursor KNOWS, so topLcp = min(col[r_prev .. r_cur-1]) - a
range-min over ~3,000 bytes instead of a 450 us text walk. That is the next change, and
it is the only available 10x+ in the finish.

## Hot-spot follow-up (2026-10-26): establish the actual product path

At starting commit `4402f71`, Kasai phase 2 ALREADY answers every run from the
loaded column band. `gx[i]` is a global run index, not a sparse-row predicate.
The two unconditional `slimLce` calls are in `!kasaiOn`; the other two are
behind explicit `kasaiAudit`. The requested sparse-row replacement is therefore
already present. This follow-up makes the interior contiguity assertion explicit
at the read and removes the unnecessary min/max endpoint reorder. No fallback,
rank source, guard, or audit was removed.

A fresh existing `seed_first_gate.py` fixture baseline passed product and debug:
SHA `98f1f6dc82befd1659112678bbc3debcf6c40ec417fcea86df47a8a51019c658`,
chi 145685, runs 192138, closure 0/0/0, fallbacks 0, STREAM_VS_BWT mismatches 0.
`XSA_FINISH_CORPUS_ACCESS phase=sweep calls=0` establishes that this starting
product path already performs zero text walks, including sparse runs.
The existing `lce-span` wall INCLUDES overlap with repair and advance; it is
not an additive sparse-LCE term. The previous 24.4 + 17.4 + 6.2 second budget
must not be read as three independent serial costs.

Baseline product 150 MB (after the fixture gate): repair 15.207 s (readband
1.130, feed 13.849), lce span 21.733 s of which only 0.007 s waited and
21.726 s overlapped, advance_witness 5.8 s. These are fresh measurements,
not a claimed optimization. Baseline evidence is under
`tools/binbwt/evidence/hotspots-baseline/`; full logs under
`/tmp/hotspots-baseline-{tiny,150mb}/`.

Fix 1 fixture gate passed the same expected SHA, chi, runs, all closure and BWT
checks, and zero product dump scratch. Product walls: repair 0.029 s,
lce span 0.037 s (wait 0.001), advance_witness 0.019 s; baseline fixture
repair 0.035, lce span 0.050 (wait 0.007), advance 0.020 s. These short timings
are noisy; the contiguity clarification is not a sparse-LCE speedup claim.
The gate now retains the existing phase/cycle/anatomy counters in its evidence
summaries instead of dropping them. Cargo source mirror/manifest refreshed and
release build passed before this gate.

Fix 1 150 MB gate also passed expected SHA
`d7eeac0f574a9a6e272732737d20b6d18a533336fea8f46f10287cb0e2b063a5`,
chi 44034231, runs 57279187, closure 0/0/0, fallbacks 0, zero BWT mismatches
and zero product dump scratch. Product walls: repair 15.013 s (read 1.213,
feed 13.582), lce span 21.988 (wait 0.003), advance_witness 6.285 s.
Sweep corpus accesses remain zero. No sparse-text-walk cost existed to remove
on this checkout. Fix 1 source and packaged manifest remain those attested
by the fixture gate; the next change instruments repair before optimizing it.

### Repair: first measured split, before any repair optimization

The first instrumented fixture gate passed expected bytes and all acceptance
checks. Its ONE product measurement (188 hashed samples / 192140 raw inputs):

| Exclusive repair term | TSC ticks/input |
| --- | ---: |
| band read/decode (all bands, unsampled) | 112.79 |
| input validation/normalization | 52.35 |
| source + normalized coalescing | **386.05** |
| seamSplice, excluding children | 48.30 |
| stamping and emitted-run accounting | 67.98 |
| require_stream_run | 22.22 |
| output/staging, excluding guard | 95.87 |

The guard is not the dominant term on this fixture. Coalescing is; output
stores are second among the transducer terms. Sampling hashes input ordinals
(mean stride 1024) to avoid aliasing the sparse writes; nested rdtsc scopes
charge exclusive ticks, including probe overhead. The tiny sample is noisy
and cannot by itself price the 150 MB path. No new instrumentation harness,
product dump scratch, full ISA, or parallel repair was introduced. Counters
are journaled as `SWEEP_CYCLES phase=repair`; an explicit
`XSA_FINISH_REPAIR_CYCLES=0` disables per-run sampling. Product walls with
counters: repair 0.043254 s, lce span 0.060509 s, advance 0.019503 s.
Evidence: `tools/binbwt/evidence/hotspots-counters/fixture-*`.

The instrument-only 150 MB gate then passed both expected containers and all
closure/stream/scratch checks. With 55,936 samples its product split is:
read/decode 44.19; raw 58.76; **coalescing 911.87**; seamSplice 58.27;
stamping 76.94; guard 24.43; output/staging 56.14 TSC ticks/input. The large
fixture confirms the coalescing path dominates this instrumentation; the guard
and staging-store hypotheses are not supported as the primary target.
Instrumented product walls: repair 18.349461 s (feed 16.853, read 1.266),
lce span 25.556722 s, advance 6.282960 s. Probes themselves perturb timing;
the noninstrumented Fix 1 measurement remains 15.013 s repair. The next
experiment keeps the source coalescer local across an ordered band and folds
ordinary, nonintersecting source runs directly into the normalized coalescer.
Both coalescers, all guards, and the exact cross-band transducer state remain;
seam-intersecting source runs continue through the full splice.

### Repair optimization: fixture green before escalation

The ordinary source-run path now performs one direct normalized append when
its half-open row span ends before (or at) the next seam class. Intersecting
runs retain the full class traversal. The source pending record is local to
the serial band loop and restored at the boundary; the delayed record and
normalized pending run still carry across bands exactly. No guard or audit
was removed, and no repair worker was added.

The first optimized fixture gate is byte-identical (expected SHA, chi 145685,
runs 192138, closure/fallbacks/mismatches all zero, product dump scratch zero).
Product repair falls from instrumented 0.043254 to 0.022513 s; feed from
0.031 to 0.015 s. Lce span 0.031572 s, advance 0.019508 s. Its 188-sample
split is read/decode 63.17, raw 68.72, coalescing 152.66, seamSplice 0.00,
stamping 72.77, guard 23.19, output/staging 119.47 TSC ticks/input. Compare
coalescing + seam together (434.35 -> 152.66); the direct append no longer
enters the seam timing scope for ordinary runs. Zero sampled seam ticks does
not mean seam work is absent: those rare class runs did not land in the sample.
The guard remains mandatory on every emitted run, sampled or not.

Existing selftests passed: STREAM_GUARD refusals=8; CHUNKED_REPAIR_ORACLE
streams=128, widths=3392, both coalescers/class flanks/padding straddle/dollar/
legal 00 and 05 covered. Full fixture gate evidence is under
`tools/binbwt/evidence/hotspots-repair/fixture-*`.

### Repair optimization: 150 MB green

Both containers match expected SHA
`d7eeac0f574a9a6e272732737d20b6d18a533336fea8f46f10287cb0e2b063a5`,
chi 44034231, runs 57279187, closure 0/0/0, walk_fallbacks 0, STREAM_VS_BWT
mismatches 0, and product dump scratch 0. Emit completed with window 100000000
and both artifacts before every finish; no overlapping emit/finish was used.

| Product term (seconds) | Starting code | Instrument-only | Optimized |
| --- | ---: | ---: | ---: |
| repair | 15.207 | 18.349461 | **11.841864** |
| repair feed | 13.849 | 16.853 | **10.165** |
| LCE span (overlapped) | 21.733 | 25.556722 | 18.802154 |
| exposed LCE wait | 0.007 | 0.004 | 0.004 |
| advance_witness | 5.752 | 6.282960 | 6.206666 |
| sparse text walks (count, not seconds) | 0 | 0 | 0 |

Optimized repair is 35.5% below the instrument-only baseline and 22.1% below
the original uninstrumented run, despite retaining the sampled counters.
The sparse text-walk wall was already absent on this checkout; the LCE span
is not additive with repair/advance and is not a standalone sparse-LCE wall.
These are single runs with page-cache/scheduler/probe noise, not extrapolated
pile-time guarantees.

| Exclusive repair term (TSC ticks/input) | Instrument-only | Optimized |
| --- | ---: | ---: |
| band read/decode | 44.19 | 50.00 |
| raw validation/normalization | 58.76 | 209.44 |
| coalescing | 911.87 | 371.09 |
| seamSplice excluding children | 58.27 | 0.01 |
| stamping/accounting | 76.94 | 73.00 |
| require_stream_run | 24.43 | 24.71 |
| output/staging excluding guard | 56.14 | 56.62 |

Coalescing + seam ticks fell 61.7%; guard and output terms remain essentially
unchanged. Raw tick attribution rose; this is not a uniform speedup across
all subterms. Counter samples are 55,936 in both runs. All original audits
remain, the product still has no full ISA or debug dump scratch, and repair
is serial. Existing `test_kasai_closure.py` also passed all negative refusals,
anchored/unanchored/walk/debug byte comparisons, and bad-anchor refusals with
the expected fixture SHA (`hotspots-repair/regression.log`).

Final review isolated the serial counter from the pre-existing experimental
checkpoint worker mode: `--repair-chunk-runs` disables repair cycle sampling
rather than copying a shared counter pointer into workers. The ordinary serial
product path is unchanged. This avoids introducing an instrumentation race
into that opt-in mode; no parallel repair mode was added or used in the gates.
After rebuilding the packaged mirror/manifest, the final fixture gate again
passed the expected SHA, chi/runs, all-zero closure/fallbacks/mismatches and
zero product dump scratch (`hotspots-final/fixture-*`).

The final binary's 150 MB product/debug gate also passed both expected SHAs,
chi 44034231, runs 57279187, closure 0/0/0, fallbacks 0, BWT mismatches 0,
independent column chi 44034231 and zero product dump scratch. This gate used
exactly the final packaged source; source manifest verification passed all
59 files and the runtime mirror matches the edited C++ byte for byte.

Final product walls (seconds): repair **8.984302**, feed 7.354, lce span
15.840102 (exposed wait 0.003, overlapped 15.837), advance_witness 6.169265.
Final fixture walls: repair 0.022149, lce span 0.030724, advance 0.019316.
Both sizes again report zero sweep corpus accesses: sparse text walks remain
zero before and after. Final 150 MB cycle split (55,936 samples): read/decode
48.71, raw 133.99, coalescing 279.46, seamSplice 0.01, stamping 72.14,
require_stream_run 24.09, output/staging 56.57 TSC ticks/input.

The final repair run is 51.0% below the instrument-only baseline and 40.9%
below the starting-code run. The earlier optimized run was 11.841864 s:
report the observed 8.98–11.84 s range rather than promising the fastest
single-run improvement. The final counter-isolation edit changes setup only,
so this variation is not credited to an additional repair algorithm change.
All evidence is in `tools/binbwt/evidence/hotspots-final/`; all green gates
were committed and pushed on `pile-week-v1`, with source manifests refreshed.
No pile-scale performance claim is made.

### Second repair pass: finer fixture attribution

The finer exclusive counters retain all guards and distinguish source character
branches, source record copies, and staging writes. Packaged fixture gate is
byte-identical (`98f1f6dc82befd1659112678bbc3debcf6c40ec417fcea86df47a8a51019c658`),
chi 145685 / runs 192138, closure/fallbacks/stream mismatches zero. Product
repair is 0.021937 s. TSC ticks/input (188 samples): decode 61.17, raw 54.89,
normalized coalescing 73.30, seam 0, stamp 71.81, guard 23.30, sink accounting
74.79, source character branch 107.45, source copy 56.81, staging 46.17.
Probe overhead is included; these small scopes are not calibrated CPU costs.
The coalescing/stamping path warrants removing its temporary 56-byte NRun
construction before considering the much smaller mandatory guard. Existing
stream-guard, repair-chunk, and sweep-monoid selftests all exit zero.
Evidence: `tools/binbwt/evidence/repair-half-instrument/`.

The finer-instrumentation 150 MB gate also passes both exact expected SHAs,
chi 44034231 / runs 57279187, closure 0/0/0, fallbacks 0 and independent
STREAM_VS_BWT mismatches 0. Product repair 10.369899 s, lce 17.368060 s,
advance 6.214468 s. Exclusive ticks/input (55,936 samples): decode 39.45,
raw 55.16, normalized coalescing 89.42, seam 0.01, stamp 71.69, guard 24.25,
sink accounting 79.32, character branch 289.55, source copy 291.51,
staging 24.39. The source-copy attribution grows markedly with the large
working set; probe/scheduler/cache effects preclude treating fixture ratios
as large-input predictions. Build work ran concurrently on other cores.

### Second repair pass: direct ordinary-source append

Ordinary source runs now populate the delayed normalized record's fields
directly; seam runs still use the existing stamped append. Both coalescers,
head/tail stamp semantics, and every audit remain. Fixture product/debug SHA
is the expected SHA, all closure/fallback/mismatch counters zero; all three
existing guard/chunk/monoid selftests exit zero. Fixture repair 0.025445 s
versus instrument-only 0.021937 s: no fixture speedup is claimed. Its split
(decode/raw/coalesce/seam/stamp/guard/sink/character/copy/staging) is
62.86/55.64/73.94/0/72.23/24.15/75.53/169.15/23.09/63.19 ticks/input.
Large-input validation will determine whether this is useful.
Evidence: `tools/binbwt/evidence/repair-half-direct/`.

Direct-append 150 MB gate: exact expected product/debug SHA, chi/runs and
zero closure/fallback/stream mismatches. Product repair **12.666650 s**,
lce 19.625701, advance 6.152293. Split: decode 44.84, raw 63.24, coalesce
76.50, seam 0.01, stamp 74.60, guard 23.81, sink 80.43, character 534.39,
source-copy 39.14, staging 24.37 ticks/input. This is a regression against
10.369899 s instrument-only and 8.984302 s starting measurement; the direct
append candidate is rejected and reverted in the next candidate. All bytes
were correct. No guard is waived to pursue the timing target.

### Second repair pass: compile away unsampled transducer probes

The direct-field append is reverted. A single per-input dispatch now selects
identical sampled/unsampled transducer template instantiations; unsampled
nested scopes use a compile-time null counter. Every normalization, seam,
coalescing, stream guard, rank stamp, fingerprint and output action remains.
The fixture gate is byte-identical with all zero closure/fallback/mismatch
counters and all three selftests exiting zero. Repair 0.021286 s; split
60.65/64.14/78.41/0/69.89/22.55/72.03/114.79/39.57/43.30 ticks/input
in the previously documented term order (188 samples). The sink's probes
are still runtime-selected. Evidence: `tools/binbwt/evidence/repair-half-specialize/`.

Transducer-specialized 150 MB product/debug gate passes exact expected SHA,
chi/runs, closure 0/0/0, fallbacks 0 and independent stream mismatches 0.
Product repair **8.802295 s**, lce 15.876573, advance 6.374982. The split
(decode/raw/coalesce/seam/stamp/guard/sink/character/copy/staging) is
43.32/64.74/85.81/0.01/71.70/23.62/79.68/288.11/134.59/24.44 ticks/input,
55,936 samples. This recovers the direct-field regression but is only 2%
below the starting 8.984302 s observation; the requested halving is not met.

### Second repair pass: propagate probe choice through the sink

The sink accepts the same compile-time measurement tag as the transducer;
unsampled guard/accounting/staging execute without timer-selection branches.
The guard itself remains unconditional. Generic/replay sinks retain their
single-argument interface; final flush uses the measured sink wrapper.
Packaged fixture SHA/chi/runs are exact, closure/fallback/mismatch counters
zero, selftests exit zero. Product repair 0.021705 s; split
67.19/77.77/84.36/0/71.70/25.43/87.34/109.15/37.77/85.96 ticks/input.
Evidence: `tools/binbwt/evidence/repair-half-sink/`.

Sink-specialized 150 MB product/debug gate passes exact expected SHA,
chi/runs, closure 0/0/0, fallbacks 0 and STREAM_VS_BWT mismatches 0.
Product repair **8.741546 s**, lce 15.323830, advance 5.871286. Split
40.23/69.95/84.36/0.01/71.92/23.97/79.34/298.42/138.80/24.07 ticks/input
(55,936 samples). Feed remains 7.370 s; this is a small improvement, not
halving. Source-character/copy attribution remains large. No simultaneous
build ran during this candidate's product timing.

### Second repair pass: outline the sampled transducer

A noinline wrapper keeps the timer-heavy sampled instantiation out of the
ordinary loop. Sampling ordinals, all audits, and the state machine are
unchanged. Packaged fixture gate passes expected bytes/chi/runs, all-zero
closure/fallback/mismatch counters, and the three selftests exit zero.
Fixture repair 0.026452 s; split
65.09/82.56/68.40/0/74.68/23.94/77.13/121.38/64.99/67.35 ticks/input.
This fixture wall regresses, so only the large gate can justify retaining it.
Evidence: `tools/binbwt/evidence/repair-half-outline/`.

Outlined-sample 150 MB gate remains byte-exact and audit-clean, but product
repair regresses to **16.901419 s** (lce 23.398593, advance 5.678931).
Split: 43.08/80.93/72.57/0.01/72.70/23.74/91.44/497.74/332.99/24.21
 ticks/input. This candidate is rejected; its wrapper is reverted next.
Symbol inspection of the prior sink-specialized binary shows the false-tag
sink still has an out-of-line operator: template visibility alone did not
ensure sink inlining. The next candidate explicitly requests inlining at
that call boundary, retaining all sink operations and checks.

### Second repair pass: enforce sink inlining

The sampled-outline candidate is reverted. The sink now requests always_inline,
so the serial transducer sees accounting/staging directly rather than paying
an out-of-line call despite having a concrete template sink. No output,
guard, or audit is changed. Fixture SHA/chi/runs remain exact, all closure/
fallback/mismatch counters zero. All four existing guard/chunk/monoid/seam-spool
selftests exit zero. Fixture repair 0.023879 s; split
76.79/80.11/74.47/0/72.98/24.04/75.85/153.72/24.36/70.11 ticks/input.
Evidence: `tools/binbwt/evidence/repair-half-inline/`.
