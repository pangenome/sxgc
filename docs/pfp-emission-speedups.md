# PFP emission speedups — the ticket list, and how to test each one

Status: **open plan**, 2026-11-26. Written to be followed ticket by ticket until
every idea here has either landed or been disproved. Every row has a measured
basis, a prediction, and the smallest arm that can falsify it.

The construction path is now cheap: at 10 GB, naming + census + rank is
**176.8 s** of a 6,919.5 s build. Everything left is the row pipeline
(`pfp_rows` → `stream-context` → `xsa_finish`), which is what this plan attacks.

## Rules that bound every ticket

1. **The product gate comes first.** Any ticket that can change emitted order
   must pass, in this order: `xsa/tests/build_pfp.py`, `gates/pfp_generated_v10_test.py`
   (pins 378,392 B / `921f4e47…` and 310,032 B / `02508df3…`),
   `xsa/tests/pfp_rows.py`, `xsa/tests/pfp_lift.py`, and then the 100 MB anchor
   pin `eac582ca…` and the 1 GB anchor pin `13cab8b6…`. A phase that gets faster
   while a pin moves is a defect, not a result.
2. **No replay.** The row producer asserts a one-pass plan
   (`!e.plan.scans && e.plan.jobs.size()==ix.bands.size()`; the refusal is
   `PFP ranges require replay`). Work may be *re-partitioned*; postings may not be
   re-read. This is why the planner-refinement ticket is closed, not open.
3. **No new defaults, no new engine.** `-j` and `-m` keep their meanings; a
   ticket either fits inside them or is refused with numbers.
4. **State the grants.** Every arm records CPU set, cgroup memory, swap,
   `-j`, `-m`, scratch and the product SHA (`gates/pfp_scale.sh`).
5. **A failed ticket is a result.** Record it in
   `gates/receipts/pfp-scale/` with the number that killed it, then stop — do not
   keep tuning a mechanism the data has rejected.

## The rungs, and which one shows which ticket

| rung | arm cost | what paces it | what it can show |
|---|---:|---|---|
| 393 KB / 6.3 MB fixtures | seconds | — | byte identity of order only |
| **producer-only**, 100 MB or 1 GB columns → `/dev/null` | ~15 s / ~6 min | nothing but `pfp_rows` | ordering and expansion *by themselves*, with no consumer stall |
| **100 MB end-to-end** (anchor `real-v10-map100`) | ~60 s | the two consumers (emission 53.2 s of 58.4 s; m20 feed alone 30.2 s) | m20 and writer tickets |
| **1 GB end-to-end** (anchor `rolling-build-1g`) | ~9 min | the producer (order 99.6 s + expand 413.7 s vs context 210 s, writer 181 s) | producer tickets end to end |
| **10 GB end-to-end** (`packed-10g`) | ~2 h | producer (order 2,037 s + expand 4,551 s) | confirmation only |

Two consequences worth keeping in mind:

* **A producer ticket cannot be judged at 100 MB end-to-end** — the consumers
  are the wall there, so a real improvement shows as nothing. Use the
  producer-only arm at 100 MB/1 GB instead, where `PFP_ROW_TIME` is the whole
  story.
* **A consumer ticket can be judged at 100 MB**, because the consumers are
  already on the critical path there. A ticket that does not move the 100 MB
  emission is dead.

### Producer-only arm (new, cheap)

The columns are `parse.u64`, `phrases.ref`, `inverse` — the prepared state. Given
a saved copy, the producer runs alone and its rows go to `/dev/null`:

```sh
systemd-run --user --quiet --scope --collect -p MemoryMax=48G -p MemorySwapMax=0 \
  -p CPUQuota=3200% taskset -c 0-31 \
  /usr/bin/time -v "$BUNDLE/pfp_rows" RAW COLUMNS 32 16G 17000000000 >/dev/null
```

`PFP_ROW_TIME` then reports `order_s`, `expand_pipe_s` (now free of consumer
stall), `gather_cpu_s`, `sort_cpu_s`, `text_cmp` and the range/wave counts. That
is the A/B signal for T1–T4. Producing the columns once per rung costs one
normal parse+rank; `$S/pfp-levels-100m-kAaLkG/prepared` already holds a 100 MB
inverse and `$S/parse-ranks-WjSNyk/parse.u32` a prepared 1 GB tape.

## The 10× question, in numbers

Measured per-unit cost at 10 GB, summed: ordering 20,490 core-s (9.13e9
offsets), expansion ~14,000 core-s (1.0e10 rows), m20 feed 3,062 s and writer
2,627 s (both serial), naming + rank ~500 core-s — **about 40,700 core-s**. Over
64 cores that is **636 s**, i.e. 10.9× below today's 6,919 s. The arithmetic
exists; four things stand in the way:

* the expansion is **one random cache line per row** — `predecessor(p)` is
  literally `text[p-1]` and `pos`/`aligned` are sequential in rank, so there is
  no structure to remove, only **memory-level parallelism to raise** (prefetch,
  more lanes): 1e10 misses at 640 in flight ≈ 1,500 s floor;
* m20 is **O(rows) in one thread** and must become O(contexts) and/or parallel;
* the **χ sweep** is a sequential 256-entry state machine over 3.47e9 runs;
* **ordering** is 2.24 µs per offset and needs a merge that is not
  rotation-based (see the rejected ticket below).

So: **3–4× is engineering inside this design; 10× needs those four to land plus
the per-row work to stop paying a random access per row.** The tickets below are
ordered by evidence, not by ambition.

## Tickets

Status legend: `open` / `landed` / `rejected` / `blocked`.

| # | ticket | side | rung that shows it | prediction | disproof (smallest) | status |
|---|---|---|---|---|---|---|
| T1 | **Prefetch the per-row lookups** in `expand`: `__builtin_prefetch(text+p_next-1)` a few rows ahead, and the next tile's first span | producer | producer-only 1 GB | expansion 4,551 → ≤1,500 s at 10 GB; producer-only 1 GB `expand` −30 % or more | producer-only 1 GB expand moves <10 % | open |
| T2 | **Ping-pong ordering**: two slot arrays, `std::merge` per level, copy back at the end; halve `slots` per grant so the second array is accounted | producer | producer-only 1 GB, then 1 GB e2e | ordering 2,037 → ~350 s at 10 GB; 1 GB ordering 99.6 → ~40 s | 1 GB ordering does not fall by ≥40 % | open |
| T3 | **Split a heavy class across lanes**: a class with millions of occurrences emits its range in ordered chunks, one lane per chunk, `seam(prev,key)` per chunk | producer | producer-only 1 GB | expansion → ~1,000 s at 10 GB | producer-only 1 GB expand does not fall by ≥30 % | open |
| T4 | **Wider row key**: 24-byte slot (16 key bytes + code), or compare 8 more bytes on a tie | producer | producer-only 1 GB | `text_cmp` 32 % → <5 %; ordering −15…25 % | producer-only 1 GB `text_cmp` stays above 15 % | open |
| T5 | **m20 counts from row contiguity**: a node's rows are contiguous, so its `count` is `row - start` at close; only `docs` accumulates per row | consumer | 100 MB e2e | feed 30.2 → ≤18 s at 100 MB | feed moves <15 % | open |
| T6 | **m20 in parallel**: split the sorted row stream at first- and second-byte bucket boundaries, build subtrees independently, concatenate in bucket order; root document accumulator merged separately | consumer | 100 MB e2e | feed /lanes; 100 MB feed ≤8 s | any pin moves, or feed does not fall by ≥2× at 32 lanes | open |
| T7 | **m1 blocks in parallel**: the 65,536-run blocks are independent (payload + directory are position-addressed) — encode on N threads, concatenate, prefix-sum the directory | consumer | 100 MB e2e | writer `add` 21 s/GB and part of `rows` overlap; 10 GB writer −30 % | 100 MB emission does not move | open |
| T8 | **Chunk the χ sweep**: save/restore the 256-entry state at run-range boundaries and sweep ranges in parallel (witnesses go to a bitmap, so order is irrelevant) | consumer | 100 MB e2e | `sweep` 56 s/GB → /chunks | χ count or m5 bytes differ from the pin | open |
| T9 | **Cheaper per-row validation** in the writer: the six-condition `pf_check` per row is most of the 98 ns row loop; keep it at packet granularity + a sampled per-row check | consumer | 100 MB e2e | `rows` 98 → ~60 ns/row | 100 MB emission does not move | open |

### Closed

| # | ticket | outcome |
|---|---|---|
| R1 | planner refinement of an oversized range | **rejected**: the build refuses `PFP ranges require replay`; refinement replays postings. `gates/receipts/pfp-scale/1g-refine-j32-m8G-REJECTED.json` |
| R2 | `std::inplace_merge` over chunk-sorted jobs | **rejected**: 11 h 44 m of CPU on 32 cores in 22 min for 1.14 GB of rows; rotation-based merging at tens of millions of records. `1g-orderwave-j32-m8G-REJECTED.json` |
| L1 | `MADV_HUGEPAGE` on the row view's anonymous columns and the arena | **landed** `0135d6d5`: 1 GB 620.3 → 549.7 s, same pins. Should be larger at 10 GB — not yet measured there |

## Ordering of work

1. **T1, T2** (producer, cheap, producer-only arm): the two that can plausibly
   take 10 GB from 6,919 s to ~3,000 s.
2. **T5, T9** (consumer, 100 MB arm): the cheapest consumer wins; both are local
   rewrites of a loop body.
3. **T3** (producer): the heavy-class fan-in, the biggest single remaining term.
4. **T7, T8** then **T6**: writer first (independent blocks), χ second (state
   chunking), m20 last (it needs the bucket split and a merge of document
   accumulators — the most delicate, and the one where byte identity is hardest).
5. **T4** last among the producers: it only matters once ordering is parallel.

Promotion rule for the plan as a whole: a 10 GB run is authorised only when the
1 GB end-to-end arm shows the intended phase moving, and the plan is finished
when either a ticket list is exhausted or the 10 GB wall reaches ~2,300 s (3×).
Beyond that the per-row random access in `expand` is the floor and 10× stops
being an engineering question.

## Receipts

Everything so far: [`gates/receipts/pfp-scale/`](../gates/receipts/pfp-scale/).
New arms go there as `<rung>-<ticket>-<grants>.json` with the decision line in
the README, so the list above can be closed ticket by ticket.
