# PFP construction at scale: 100 MB → 1 GB → 10 GB

`xsa build --pfp`, one ordinary publication per rung, driven by
[`gates/pfp_scale.sh`](../../pfp_scale.sh) (a user cgroup scope with a fixed CPU
set, `MemorySwapMax=0`, metered by [`gates/pfp_scale_arm.py`](../../pfp_scale_arm.py)).
Every arm that produced a product matched its **pinned whole-v10 SHA**; the pins
come from the earlier producers, so this is byte identity, not a self-comparison.

Scope: construction time and workspace. Not a container-format change, not a
`xsa build` default change, not a new engine.

## What the measurements found, and what was changed

Each arm below is the same input, grants and product; only the named mechanism
changed. All of them are inside construction and leave the v10 bytes identical.

1. **Phrase naming sorted on the text, one run, one thread.** The sort arena was
   sized by *symbols* per worker, so a 3.2 M-occurrence input produced **one**
   run and `std::sort` ran on one core (30.7 s of a 39 s naming phase at 100 MB,
   independent of `-j`). Occurrences now also carry the first **two eight-byte
   words** of their finite window (two whole symbols at an ID level), so most
   comparisons are integer comparisons; the exact comparator runs only when the
   keys agree. A short window is zero-padded, so a proper prefix still sorts
   first, and a window that runs past the end of the file is left unkeyed.
2. **Parallel chunk sort with a linear merge.** The occurrence set is sorted in
   `4 x workers` chunks and merged pairwise into a second buffer. The old
   path used the rotation-based in-place merge, which cost 22.9 s of a 25.0 s
   sort at 1 GB.
3. **The ID-tape scatter was capped at 16 MiB.** The tape is written by a random
   permutation of text-order ordinals; with a slab smaller than the tape, most
   writes became a `pread`+`pwrite` pair (7.7 s at 100 MB). The slab is now sized
   from the grant and the tape, and a full-coverage slab never reloads a page.
4. **Row ordering compared text for 68% of its comparisons.** The 8-byte arena
   slot held only 3 spare bytes for a key. The arena record is now a 16-byte
   `{key, code}` — exactly the 16 bytes per descriptor the planner already
   reserved — and the key is the eight bytes past the band's known prefix.
   At 1 GB this cut text comparisons from 18.9 G to 7.5 G and sort CPU from
   2,883 s to 1,372 s.
5. **The row planner's slot count was estimated from a pre-plan.** The per-band
   layouts, the tile bookkeeping and the arena all scale with the *real* range
   count and slot count, so a large build overshot its own grant and refused
   (`ordered stream array grant`) while a small one must not lose its slots to a
   fixed reserve. `Emit::prepare` now measures the exact array need and drops
   slots by exactly the excess (the planner refines an oversized prefix), so the
   fit is exact at every grant.
6. **Naming read the input through a 4 KiB page cache.** At 10 GB the paged
   reader spent **1,041 s of a 1,128 s naming phase** in 886 M `pread` calls.
   The stage now maps the input read-only; a comparison is a memory read.
   Naming fell from 1,128 s to 126 s at 10 GB, byte-identically.

## The ladder

### 100 MB real v10 — `real-v10-map100-S4bZm0/inputs/raw`, 100,000,503 B

Pin `eac582ca1e51ead51239b60b1506c44cacf9b6713b675b16003f57fc4f3f98bd`
(87,192,432 B, 100,000,504 rows). Cores 64–95, 8 GiB cgroup, no swap.

| arm | `-j` | `-m` | wall s | CPU/wall | naming scan/sort/merge s | rows order s | emission s |
|---|---:|---:|---:|---:|---|---:|---:|
| four workers, first ladder | 4 | 320M | 144.4 | 2.20 | 0.72 / 30.20 / 8.55 | 56.4 | — |
| same, larger arena | 4 | 2G | 134.1 | 2.33 | 0.70 / 30.69 / 7.25 | 49.7 | — |
| more workers, same arena | 16 | 2G | 101.7 | 3.15 | 0.69 / 31.06 / 8.41 | 19.3 | — |
| 32 workers | 32 | 2G | 89.0 | 3.73 | — | 10.0 | — |
| + scatter slab | 32 | 2G | 67.2 | 4.68 | 0.48 / 4.41 / 4.26 | 10.0 | — |
| + 16-byte slots and naming keys | 32 | 2G | **58.4** | 4.03 | 0.57 / 1.05 / 2.71 | 6.7 | 53.2 |

The last row is the point where construction stops being the cost: 4.5 s of
naming, 6.7 s of row ordering, and 53.2 s of emission that is paced by the two
single-threaded consumers (below).

### 1 GB — `rolling-build-1g-U4MRhu/in/raw`, 1,000,000,665 B

Pin `13cab8b64fb728b9d2a6b5701cd498b10f573d7d8d2ce054cdda98cb0a517457`
(805,063,696 B, 1,000,000,666 rows). Cores 64–95, 32–40 GiB cgroup, no swap.

| arm | `-j` | `-m` | wall s | naming s | rank s | rows order s | context feed s | writer work s | SHA |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| v1 (slab fix only) | 16 | 16G | 855.1 | 152.9 | 6.4 | 320.6 | 235.1 | 181.4 | ✓ |
| v3 (+ keys, 16-byte slots) | 32 | 16G | 490.6 | 60.1 | 1.5 | 90.3 | 201.7 | 181.4 | ✓ |
| v4 (+ mapped naming) | 32 | 16G | 572.9¹ | **14.4** | 0.6 | 102.3 | — | — | ✓ |
| final release, installed bundle | 32 | 48G | 620.3¹ | **14.1** | 0.4 | 106.2 | — | — | ✓ |

¹ v4 ran concurrently with the 10 GB arm and the final release ran on cores
0–31, which carry other load; their emissions are inflated, their naming is not.

### 10 GB — `packed-10g-l2-b1-t4-r1-GWrxev/in/corpus`, 10,000,000,000 B

Pin `ecb2e766af6984c5f5b898d8c8f889af7b1cb19b8685365f2a75b5081469e07d`
(7,383,791,080 B, 10,000,000,001 rows). Cores 64–127, 200 GiB cgroup, no swap.

| arm | `-j` | `-m` | wall s | parse s | P1 rank s | rows order s | expand (incl. pipe) s | context feed s | writer work s | SHA |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| v4 | 64 | 48G | **6,919.5** | 168.5 | 3.9 | 2,036.9 | 4,551.0 | 2,985.9 | 2,616.3 | ✓ |

Full v10 product 7,383,791,080 B; 10,000,000,001 rows; 3,474,883,891 runs;
9,129,290,112 dictionary offsets in 394 ranges, each counted/routed/ordered
once; cgroup peak 60,832,174,080 B; sampled scratch 13,411,365,005 B against the
2N = 20,000,000,000 B target; 5.08 cores busy on average of the 64 granted.

For comparison the old raw-position producer took **21,542 s** on this corpus
with four workers (`gates/receipts/build-stream/10g.json`), and its
`stream+finish` alone was 21,389 s. Construction (parse + rank) fell from about
2,180 s in that path to **176.8 s**.

### Where the 10 GB wall actually is

`raw → prepared` is 176.8 s; `prepared → published` is 6,740 s. Three separate
effects, each measured:

1. **Row ordering ran at 8.4 effective lanes** (`sort_cpu_s` 17,062 over a 2,037 s
   phase). A range is never cut inside a two-symbol cell, and on this corpus the
   largest cell holds 212 M descriptors, so a wave's wall is set by its biggest
   range. 32% of comparisons still fell through to the text (272.5 G key, 88.4 G
   text) because the eight-byte representative key is weak on repetitive text.
2. **Row expansion took 4,551 s of `expand_pipe` for 10 G rows while the whole
   build averaged 5.08 cores of the 64 granted.** Tiles are handed to `team`
   lanes but drained by one ordered consumer through three-packet rings; a tile
   that emits millions of occurrences blocks its lane *and* the drain, so the
   other lanes stall behind it.
3. **The two consumers are single-threaded per row** — m20 feed 2,986 s, the v10
   writer 2,616 s (1,375 s of it the per-row unpack/run loop, 980 s the χ sweep).
   They were *starved*, not saturated (writer read-blocked 5,292 s).

So above about 8-16 workers this pipeline is no longer CPU-bound: more workers
cannot shorten phases that are paced by an indivisible range, a fan-in leader, or
a single-threaded consumer.

## Huge pages (kept)

This host sets `transparent_hugepage/enabled=madvise`, so anonymous scratch stays on
4 KiB pages unless asked. The row view's columns and the arena are read at random over
gigabytes, which is a TLB miss per row. `rawview::Mem::open` and the arena now request
`MADV_HUGEPAGE`. Same 1 GB arm, same grants, same product SHA
([thp](1g-thp-j32-m48G.json) vs [final](1g-final-j32-m48G.json)): wall 620.3 -> **549.7 s**,
row ordering 106.2 -> 99.6 s, expansion 478.6 -> 413.7 s. A second pair at `-m 8G`
([thp](1g-thp-j32-m8G.json), 651.6 s) shows the same direction under a tighter arena.
The gain should be larger at 10 GB, where the columns are ten times bigger; it is not
measured there yet.

## A rejected fix, and why it matters

The obvious way to fix the 8.4-lane row ordering is to let the *planner* refine an
oversized range down to the wave target. That was implemented and measured
([1g-refine-j32-m8G-REJECTED.json](1g-refine-j32-m8G-REJECTED.json)): the build
**refused** with `PFP ranges require replay`. The PFP row producer asserts a
one-pass plan (`!e.plan.scans && e.plan.jobs.size()==ix.bands.size()`), and planner
refinement replays the refined job's postings. That guard is the no-replay law for
this path, so the route is closed rather than relaxed.

The open route with the same goal is a **parallel sort inside one job**: gather once,
sort the job's arena in `4 x workers` chunks, then merge in place. It replays nothing,
so the contract holds.

## Next tickets, in the order the numbers above justify

1. **Sort one job with several lanes.** The largest two-symbol cell (212 M
   descriptors here) is indivisible to the *router* and may not be refined
   (that replays postings), but its slots can be gathered once, sorted in
   `4 x workers` chunks and merged in place, exactly as the naming phase does.
   Ceiling: row ordering 2,037 -> about 400 s, so the 10 GB wall falls to the
   expansion.
2. **Drain tiles without a single ordered leader.** Give each lane a larger
   out-of-order budget, or emit per-lane ordered segments and interleave by tile
   sequence, so one occurrence-heavy tile cannot stall the other lanes.
3. **Widen the representative key** (or compare 8 more bytes on a tie) so the
   32% text fallback in row ordering shrinks.
4. **Make the two consumers parallel.** The m20 feed is a per-row state machine
   with a per-level document accumulator; the writer's m1 blocks are independent
   and could be encoded by several threads, leaving only the χ state machine
   serial.

## Reproduction

```sh
# one metered arm (user cgroup scope, fixed CPU set, no swap, product SHA check)
gates/pfp_scale.sh 64-127 200G 6400% xsa/target/release/xsa RAW MAP OUTDIR LABEL 64 48G PIN
```

The 10 GB arm used `XSA_PFP_TOOLS` (a hand-built stage directory from the same
sources) while iterating; the receipt below records the exact stage hashes. The
installed bundle path never sets it.
