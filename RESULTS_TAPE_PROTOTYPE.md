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
