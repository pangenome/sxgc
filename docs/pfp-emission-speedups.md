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

### First producer-only measurement (100 MB, 16 lanes, `-m 2G`, rows to `/dev/null`)

`PFP_ROW_TIME order_s=9.57 expand_pipe_s=**2.00** gather_cpu_s=11.5 sort_cpu_s=79.6
key_cmp=2.45G text_cmp=0.54G waves=3 tiles=61,962 ranges=35`.

The producer's **own expansion is 2.0 s for 100 M rows**. In the same input's
end-to-end arm `expand_pipe` was **44.3 s**. So almost all of that phase is the
producer *waiting for its consumers*, not producing rows — and by extension most
of the 10 GB `expand_pipe = 4,551 s` is stall, not work. Two consequences:

* **T3 (heavy-class fan-in) is not the first thing to do.** The expansion is
  already fast; it is idle because the consumer chain is slow.
* **The ordering is the producer's real term** (9.6 s here, 2,037 s at 10 GB) and
  it does not overlap with the consumers at all, because a wave sorts before it
  emits. T2's gain is therefore close to 1:1 on the wall.

**Revised priority: consumers first (T5, T9, T7, T8), then T2, then T1/T3/T4/T6.**

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

## Better ideas than the ticket list

The tickets above make each phase faster. These make the *shape* faster, and the
measurements say the shape is where the wall is: `expand_pipe` 4,551 s is stall,
the two consumers are single-threaded sums over 1.0e10 rows (m20 3,062 s, writer
2,627 s), and the ordering (2,037 s) does not overlap with either.

**I1 — K independent row pipelines, one per range group.** The row stream is
already produced *range by range*, and each range is internally ordered with its
seam carried in `prev`. So the producer does not need one downstream chain; it can
hand consecutive range groups to K `(context, writer)` pairs and concatenate their
outputs. That divides **both** serial consumers by K instead of making them
faster. What it needs: (a) cut every group boundary at a *run* boundary — the
writer tracks the previous byte, so the producer can end a group where the byte
changes and no run is split; (b) merge the per-group m1 payloads, m12 `runStart`
and m13 `dir*` arrays by run index; (c) pass the χ state (256 `rr_len`/`rr_pos`
entries) from group k to group k+1, or chunk it as in I4; (d) merge the per-group
m20 tries. Projection at K=8: emission 6,740 → about 1,500 s.

**I2 — radix ordering on the 8-byte slot key.** The sort pays 17,062 core-s for
2.7e11 comparison calls on a key that is an unsigned 64-bit word. Three counting
passes over 16-byte slots (8 bits each on the high bytes) replace the comparison
sort: the traffic is about 440 GB, tens of seconds at machine bandwidth, against
~270 s of pure comparison even at 64 lanes. Ordering 2,037 → ~250 s. It also
retires T2 and T4 (a longer key is free when the sort is a counting sort).

**I3 — m20 over context runs, not rows.** A context's rows are contiguous in
suffix order by construction. So a single pass can compute a node's `count` as
`row − start` and merge its document set once per context run, instead of
touching all eight levels for all 1.0e10 rows. This is T5 taken to its end:
O(contexts), not O(rows), with the per-row work reduced to one compare.

**I4 — χ as a parallel scan.** The sweep's state is a per-char running maximum
(`rr_len`, `rr_pos`, and the activity flags). Running maxima compose
associatively, so a scan over K chunks yields all K boundary states, after which
the K chunks emit witnesses in parallel (witnesses land in a bitmap, so their
order is irrelevant). This removes the 980 s serial floor that would otherwise
cap I1.

**I5 — fold the m20 builder into each pipeline.** It is a separate process only
because it was added later; inside a pipeline it saves one 120 GB pipe hop and one
scheduler hop per group.

**Where the four land**, all at 10 GB and K=8: naming 177 s + ordering ~250 s +
expansion ~500 s + consumers ~1,000/8 ≈ 125 s, plus the χ scan → **roughly
900–1,200 s, i.e. 6–7.5×**, and ~700 s (10×) if the χ scan and the expansion's
prefetch land too. That is a redesign of the emission stage's shape, not a tuning
pass, and it is the only route to the 10× that the per-unit numbers allow.

Order of attack if these are adopted: **I2** first (self-contained, no output
change, 100 MB producer-only arm), then **I3** (100 MB arm, pins), then **I1+I5**
(the redesign), with **I4** either before I1 (if χ is to be chunked) or after (if
a sequential handoff is acceptable at first).

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
| T9 | ~~Cheaper per-row validation~~ | consumer | 100 MB e2e | `rows` 98 → ~60 ns/row | — | **rejected** (R4): the fence is free |

### Closed

| # | ticket | outcome |
|---|---|---|
| R1 | planner refinement of an oversized range | **rejected**: the build refuses `PFP ranges require replay`; refinement replays postings. `gates/receipts/pfp-scale/1g-refine-j32-m8G-REJECTED.json` |
| R3 | chunk-sort + `std::merge` ping-pong ordering (T2, first attempt) | **inconclusive, reverted**: with 1,557 pieces of 17,835 records the 100 MB producer-only arm did not reach the post-sort point in 90 s where the baseline orders the same input in 9.6 s. Not isolated yet; the next step is to time one piece sort and one merge level in isolation. No code kept. |
| R4 | packet-granularity fence instead of the per-row six-condition check (T9) | **rejected**: rows 10.51 -> 10.68 s at 100 MB, wall 50.48 -> 49.89 s, pins unchanged. The row loop's cost is run bookkeeping, not validation. `100m-t9-nopcheck-j16-REJECTED.json` |
| R2 | `std::inplace_merge` over chunk-sorted jobs | **rejected**: 11 h 44 m of CPU on 32 cores in 22 min for 1.14 GB of rows; rotation-based merging at tens of millions of records. `1g-orderwave-j32-m8G-REJECTED.json` |
| L1 | `MADV_HUGEPAGE` on the row view's anonymous columns and the arena | **landed** `0135d6d5`: 1 GB 620.3 → 549.7 s, same pins. Should be larger at 10 GB — not yet measured there |

## Ordering of work

1. **T5** (consumer, 100 MB arm, ~60 s): the only consumer term that paces the
   100 MB rung. T9 is closed — the writer's fence turned out to be free, and its
   whole own-work share at 100 MB is 19.5 s of a 50.5 s build with 37.7 s
   read-blocked.
2. **T7, T8** (consumer): m1 blocks in parallel, then the χ sweep in chunks.
3. **T2** (producer): the ordering is the producer's only large term and does not
   overlap with the consumers, so its gain is close to 1:1 on the wall.
4. **T1, T3, T4** (producer): prefetch, heavy-class fan-in, wider key — these
   matter only once the consumers stop pacing the emission.
5. **T6** (m20 bucket parallelism) last: it needs the bucket split and a merge of
   document accumulators, and it is where byte identity is hardest.

Promotion rule for the plan as a whole: a 10 GB run is authorised only when the
1 GB end-to-end arm shows the intended phase moving, and the plan is finished
when either a ticket list is exhausted or the 10 GB wall reaches ~2,300 s (3×).
Beyond that the per-row random access in `expand` is the floor and 10× stops
being an engineering question.

## Receipts

Everything so far: [`gates/receipts/pfp-scale/`](../gates/receipts/pfp-scale/).
New arms go there as `<rung>-<ticket>-<grants>.json` with the decision line in
the README, so the list above can be closed ticket by ticket.
