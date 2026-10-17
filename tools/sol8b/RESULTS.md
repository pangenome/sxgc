# sol8b: pricing stop for independent whole-list boundary searches

Branch `sol-8b-bucketed-fronts`, base `f74a5e8`. **No bucketed producer was
implemented.** The requested tail-work model favors d=8, but sampled boundary
searches on the repaired compact chunk lists make the literal independent
(chunk,bucket) whole-list binary-search method unattractive. This stop concerns
that search method; it does not establish a negative for sparse frontier,
finger/galloping search or an enlarged decoded-group cache.

The measurement uses the exact 1,000,000,665-byte production-remapped 1GB
fixture and the eight retained sol7e chunks. No text or suffix positions are
written by the census: two RAM banks hold numeric keys and contributor masks
(32n bytes total). Existing sanctioned chunk positions and inherited repair
are reused in the comparator/boundary measurements. D2 is not amended.

**Exact contributor census.** Bucket totals include distinct finite short
suffixes; legal NUL extensions do not collide with them. One contributing
chunk permits literal slice emission even when bucket mass exceeds one.

|d|buckets|one-chunk suffix mass|all-eight suffix mass|largest bucket|mass-weighted log2 C|
|--:|--:|--:|--:|--:|--:|
|4|5,785,342|7,195,218|965,196,773|19,127,265|2.954464|
|6|43,957,450|52,441,368|850,803,822|15,921,967|2.747784|
|8|122,563,721|134,277,602|699,645,723|13,568,990|2.426974|

Full contributor-count bucket and mass histograms are in `histogram-1b.jsonl`.
The initial global-mean sensitivity columns in that file are explicitly proxies,
not the final conditional-tail projections below.

**Requested work expression.** The instrumented inherited SERIAL HEAP walk
counts every comparison's verified tail symbols separately for each common
4/6/8-byte prefix. Every bucket with C>1 received observations: unobserved
merge mass is zero at all three widths. For each bucket, multiply its exact
mass * log2(C) by its observed baseline mean tail-symbol cost. This evaluates
the requested expression without substituting the unrelated global LCE mean.
A mismatch symbol is counted consistently with the baseline; finite end has
no mismatch symbol. The baseline operand distribution is a MODEL for the
future bucket heap, not a measured new heap trace or exact wall prediction.

|d|projected comparisons: sum mass log2 C|projected tail symbols|vs 60,485,597,061|certified-prefix savings in this model|
|--:|--:|--:|--:|--:|
|4|2,954,465,517|38,128,346,504|-36.963%|11,817,862,068|
|6|2,747,785,689|33,066,458,839|-45.332%|16,486,714,135|
|8|2,426,975,614|28,935,990,831|-52.161%|19,415,804,911|

**Choose d=8 for the tail-comparison objective.** There is no qualifying
launch width for the independently searched boundary scheme priced here.
The model excludes preparation, boundary probes, prefix-key construction,
per-bucket heap initialization and publisher/finish work.

**Pile log tax and explicit missing tail cost.** For 1310 independent one-GB
chunks, take the observed per-GB bucket mass b as a Poisson occurrence rate:
E[C]=1310*(1-exp(-b)). The table uses log2(E[C]), not E[log2 C]. Hot buckets
approach log2(1310)=10.355351 rather than retaining their eight-way log tax.
These are distribution assumptions, not a pile census.

|d|mass-weighted log2 E[C]|observed-tail subtotal, symbols per fixture-equivalent GB|suffix mass without pile-tail observations per GB|
|--:|--:|--:|--:|
|4|10.353449|132,672,560,001|7,195,218|
|6|10.336108|120,785,420,071|52,441,368|
|8|10.294291|114,823,692,704|134,277,602|

The full pile expressions are 1310*(132,672,560,001 + U4),
1310*(120,785,420,071 + U6), and 1310*(114,823,692,704 + U8).
Ud is the unmeasured tail cost for buckets that have only one contributing
chunk in this fixture but multiple contributors under the pile occupancy
model. **The subtotals are not full pile symbol forecasts.** These buckets
have no cross-chunk baseline comparisons from which to infer their tail LCE.
Unseen prefixes and distribution shift add uncertainty. A full pile total
cannot honestly be recovered from this one fixture and these observations.

**Boundary price, measured before building a producer.** Each test samples
1024 full-width prefix keys from deterministic corpus coordinates, deduplicates
and sorts the keys, then independently lower_bounds every key in each COMPLETE
REPAIRED list. It uses existing PosOverlay::at / compact decoder, with its
16-slot group cache. No position array is introduced. Exact search/probe/byte
counts, elapsed seconds and result checksums are in `boundary-1b.jsonl`.

|d|unique sampled keys|searches|position probes|sample seconds|ms/search|
|--:|--:|--:|--:|--:|--:|
|4|912|7,296|196,433|24.217553|3.319292|
|6|992|7,936|213,692|26.271503|3.310421|
|8|1,013|8,104|218,220|26.926695|3.322642|

|d|full eight-way directory searches|serial seconds extrapolated|seconds / ideal P=8|ratio to supplied 291s|
|--:|--:|--:|--:|--:|
|4|46,282,736|153,625.9|19,203.2|65.99x|
|6|351,659,600|1,164,141.4|145,517.7|500.06x|
|8|980,509,768|3,257,883.4|407,235.4|1399.43x|

At pile fan-in, even retaining only this observed directory gives 7,578,798,020,
57,584,259,500 and 160,558,474,510 independent searches, respectively. Pile
chunks are larger than these 125MB chunks, and unseen buckets are not priced.
These counts explain the search tax; no pile time forecast is asserted.

This is a mechanical extrapolation, NOT a complete-directory measurement,
statistical bound, or measured new wall. Keys were sampled by suffix mass,
not uniformly over buckets. Host work and the conditional walk overlapped
these microbenchmarks. Locality, changed bounds, per-worker ownership, front
sampling or a larger decoder cache can change the result. Nonetheless the
literal search method has a large enough adverse price to exercise the
operator-authorized stop before a producer build. A monotone/sparse boundary
algorithm needs separate pricing; the tail-work result supports that avenue.

**Measured old versus new, gates verbatim.** The instrumented old walk exactly
reproduces all banked work counters, including preparation:

```text
CROSS_STAGE_WORK stage=heap pass=0 comparisons=3674539686 symbols_compared=60485387922 probes=1987510308
CROSS_STAGE_WORK stage=WALK pass=0 comparisons=3674629435 symbols_compared=60485597061 probes=1987510452
HYBRID_BASELINE comparisons=3674629435 symbols=60485597061 rows=1000000665
```

New hybrid comparisons/symbols/wall: NOT MEASURED; the new values above are
projections. The 291s P=8 baseline is supplied, not remeasured here. The
instrumented serial HEAP took 678.397s on this shared host; it is neither a
new producer wall nor a causal comparison to P=8. Pricing requests 96 threads,
uses CROSS_POOL_SLOTS=33554432 and caps address space at 100GiB. The inherited
serial walk remains one output worker. No quiet-box claim is made.

```text
HYBRID_PRICING_ORACLE_PASS cases=42 d=4,6,8 NUL=1 byte05=1 wrap_prefix=1 periodic=1 full_alphabet=1 nonidentity_remap=1 finite_suffixes=1 contributor_counts=exact mass=exact log_tax=exact
HYBRID_CONDITIONAL_LCE_ORACLE_PASS cases=18 d=4,6,8 NUL=1 byte05=1 wrap_prefix=1 periodic=1 finite_ends=1 hash_tail=1 counters=brute_exact
HYBRID_CONDITIONAL_LCE_ORACLE_PASS cases=18 d=4,6,8 NUL=1 byte05=1 wrap_prefix=1 periodic=1 finite_ends=1 hash_tail=1 counters=brute_exact
HYBRID_PRICING_ATTEST_PASS d=4,6,8 census=exact mass_conserved=1 conditional_coverage=accounted baseline_counters=exact repaired_boundary_samples=1 production_sources=unchanged product_gates=NOT_RUN pricing_stop=1
Full D5 producer/product oracle suite: NOT RUN (pricing stop; no producer)
100MB chi=29716349 + bank cmp: NOT RUN (pricing stop)
1GB chi=283296933 + bank cmp: NOT RUN (pricing stop)
1GB hybrid 96t/100GiB symbols/comparisons/wall: NOT RUN (pricing stop)
```

The conditional oracle runs cover RAM cache and CROSS_NO_POOL file windows.
They include hash-tail verification and compare exact counters with brute LCE.
They validate measurement, not a bucketed producer. The first tiny census
oracle exposed non-JSON infinity for zero-comparison fixtures; the tool now
emits null for their undefined break-even mean and all 42 cases pass.

**Landed measurement and residual risks.** `tools/sol8a/measure.cpp` dispatches
`--hybrid`; `tools/sol8b/pricing.hpp` performs the exact census/model reduction.
`build_pricing.py` generates a measurement-only instrumented translation unit;
`lce_measure.hpp` holds the conditional counter and repaired-boundary probes.
Production native and runtime sources remain byte-identical to base. There is
no position materialization amendment, bucket producer, transport change or
new persistent product format. Independent full-list search is the negative;
finger/galloping search, sparse frontier discovery, larger caches, pile tails
and new product correctness/performance all remain open. Binary search must
use repaired global chunk order; raw local cyclic order is insufficient.

Evidence: `conditional-1b.jsonl`, `conditional-work.log`, `validation.json`,
`provenance.json`, and `PRICING_DESIGN.md`. Scratch is exclusively sol8b-prefixed;
the active 100GB run's scratch was not opened or changed. The first optional
conditional trial used the smaller default pool and was terminated; none of
its partial counters or timing is substituted for the completed final trial.
`contact_supervisor` was searched first in the callable catalog and was absent;
no substitute external messages or supervisor-approved scope change is claimed.

Milestones: d0987c8 (measurement/instrumentation), 04b2214 (census/oracles),
ab752af (boundary price). Final evidence/report is committed immediately after
these milestones; `git log --oneline f74a5e8..HEAD` lists the complete branch.
