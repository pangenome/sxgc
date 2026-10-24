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
  rle-BWT + rungs that `pile_finish` consumes, so no container has been built
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
