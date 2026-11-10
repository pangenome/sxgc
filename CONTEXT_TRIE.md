# CONTEXT_TRIE (m20): the top of the suffix tree, materialised

2026-11-10. Branch `feat/context-trie`. Read MEMORY_OPS.md, CONTEXT_DEPTH.md and
VIRTUAL_TREE.md first. This page reports what was built, what it costs, and what it
does **not** do.

## What it is

A container member (m20, codec 20, member format version 1) that answers, for a
context (a byte string), in O(depth) with binary searches inside child lists:

- how many times the context occurs (`trie-stat`, `branch-of`),
- the EXACT next-byte histogram of the context, END included (`branch-of`),
- how many 0x1E spans (documents) it appears in, and a heavy-document candidate,
- and what the deepest supported sub-context of a larger context is (`relax`).

The root (depth 0) is always present, so `branch-of ""` is the byte histogram of
the whole corpus. A context that is absent returns an empty answer with a flag; it
is never turned into a scan.

## Why it cannot be derived from the container at query time

A context's children are delimited by the ranks where LCP drops below its depth,
and a v6/v7 lite container has no LCP column and no per-row SA column (m7 is a
256-byte permutation; m11 is one sampled run-tail SA value per 1024 runs). Scanning
a wide node's interval is not a plan: a depth-1 context has billions of occurrences
(VIRTUAL_TREE.md prices the LCP traffic of that scan at 46.95 TB/s at the 150k/s
target). The one thing the container does cheaply is *backward* search, which is
the wrong direction for "what comes next".

So the trie is built by the ordered sweep, which is the direction the suffix array
is already in.

## How it is built

Suffixes arrive in sorted order. One pass keeps a stack of the current path
(<= D entries) and, for each suffix, descends from the longest common prefix with
the previous suffix. A node's occurrence count is its **row span**
(`row_at_close - row_at_open`), so no per-suffix counter is incremented anywhere:
the pass is O(1) amortised per suffix and its state is O(D) plus the node store.
Children come out in ascending byte order for free, which is both the exact branch
histogram and the lookup order. END is code 0 and real bytes are code `1+b`, so the
stored code order IS lexicographic order.

Inside `xsa build` the band producer holds (row, SA value, text) and can feed this
builder directly. Outside it - which is how this member is produced today - the
ordered stream is reconstructed from the container:

1. **Dense run columns** (`Dense::build`): m1 is decoded ONCE, in parallel, into
   flat `heads u8[]` + `lens u32[]` plus a per-char occurrence table sampled at
   block boundaries. A query-kernel LF step pays a block decode (measured 8 us,
   and the LRU does not help a random walk); a dense step is two cache-shaped
   scans.
2. **The LF cycle**: `LF` maps row `i` to the row of `SA[i]-1`, so one cycle visits
   every row exactly once and yields the whole suffix array at one LF step per row.
   `K` breakpoints (one `locate` each, ordered by position) partition the cycle
   exactly, so the walk is parallel and disjoint.
3. **The sweep** over the resulting `sa[row] = SA[row]` (u40, 5 B/row), reading the
   D+1 prefix bytes of each suffix from a read-only mmap of the corpus and the
   0x1E span id from a per-256-byte rank array.

Nothing text-shaped is written to disc. The suffix array can be cached
(`--sa-cache`) because the curve needs many sweeps of one stream; the cache is
5 B/row and is not part of any container.

## The parameter curve (150 MB, n = 149,998,882, `/mnt/nvme2n1/erikg/ctx-trie/t150m.txt`)

One ordered stream, one sweep per cell, exact serialised bytes per cell.

| depth D | min_support K | nodes | edges | bytes | ratio_vs_n | B/node |
|---:|---:|---:|---:|---:|---:|---:|
| 16 | 10 | 15,986,374 | 70,052,969 | 1,904,270,232 | 13.49 | 119.1 |
| 16 | 100 | 1,276,404 | 14,655,050 | 89,394,408 | 0.596 | 70.0 |
| 16 | 1000 | 109,666 | 2,736,099 | 10,628,224 | 0.0709 | 96.9 |
| 16 | 4000 | 25,032 | 922,799 | 3,549,304 | 0.0237 | 141.8 |
| 16 | 10000 | 8,642 | 421,735 | 1,620,464 | 0.0108 | 187.5 |
| 8 | 100 | 779,020 | 9,847,688 | 55,910,600 | 0.3727 | 71.8 |
| 8 | 1000 | 94,493 | 2,405,363 | 9,317,624 | 0.0621 | 98.6 |
| 8 | 10000 | 8,508 | 415,264 | 1,596,168 | 0.0106 | 187.6 |
| 4 | 100 | 143,130 | 2,880,761 | 13,035,056 | 0.0869 | 91.1 |
| 4 | 1000 | 30,988 | 1,090,775 | 3,978,160 | 0.0265 | 128.4 |
| 4 | 10000 | 5,814 | 312,357 | 1,189,680 | 0.0079 | 204.6 |

The first row is the one that was wrong: **a shallow trie is small in depth, not in
node count.** 16 M of 150 M suffixes start a context that repeats at least 10 times,
so D=16/K=10 is 13.5x the corpus. Support, not depth, is the knob.

Coverage decides the default (measured on the 10 GB corpus, occurrence counts of
random contexts by depth):

| depth | median count | >=1e3 | >=1e4 | >=1e6 |
|---:|---:|---:|---:|---:|
| 8 | 2,815 | 59.6 % | 37.6 % | 2.5 % |
| 16 | 3 | 6.2 % | 2.2 % | 1.1 % |
| 24 | 1 | 1.5 % | 0.9 % | 0.5 % |
| 32 | 1 | 0.6 % | 0.5 % | 0.2 % |

At depth 16 with min_support 1000 the trie describes 6 % of positions; at depth 8 it
describes 60 %. CONTEXT_DEPTH.md measured 3.89 bits at depth 1, 1.29 at 8, 0.50 at
16, so depth 8 is where coverage and signal coincide.

**Default: `--depth 8 --min-support 1000` (ratio 0.062 at 150 MB).** The
statistically-tidier cell `--depth 16 --min-support 10000` is 0.011 and covers 1.1 %
of positions; both are one flag away and both are journaled
(`CONTEXT_TRIE ... depth= min_support=`). No single number is claimed to be "the"
right one.

## The record (member format 1)

Every fixed-width slot of the first draft is gone. Measured at depth 16 / K 1000,
the first draft was 112 B/node and 19 B/edge; that store was 2/3 edges and 1/3
fixed-width waste.

| field | cost | why it is cheap |
|---|---|---|
| node: depth u8, child count u16, flags u8 | 4 B fixed | nothing else is fixed |
| node: first-edge offset | ~1 B (packed, mostly 0) | post-order makes it monotone, so it is a delta |
| node: occurrence count | ~2.5 B (packed) | block-packed u64, exact 64-bit escapes |
| edge: child code | 1.13 B | 9-bit packed (0 = END, 1+b = byte) |
| edge: child node id | ~1-2 B | parent minus child: post-order puts a child immediately before its parent |
| edge: child count | ~2.5 B (packed) | exact, same codec |
| doc: distinct count, heavy candidate, first id | ~4-8 B/node, **only above `--docs-min-support`** | see below |

All counts are exact: the block codec is mode 0 / u8+escape / w-bit+escape with the
all-ones field as the escape, and an escaped value keeps all 64 bits. A Pile count
of 4.9e9 and a fixture count of 3 are the same format. The codec has an adversarial
selftest (all-zero, one outlier, the 254/255 boundary, u32/u64 boundaries, random)
through the exact random-access rule the reader uses.

**Document summary.** Exact distinct-document count plus the exact id list while a
node holds <= 8 distinct documents (the useful case for `project`/`select`); beyond
that the node carries a 64-register HLL estimate (13 % standard error, stated) and a
Misra-Gries 2-counter heavy-document **candidate** (labelled as a candidate). It is
computed only for nodes at or above `--docs-min-support` (default 1000): below that,
`project` answers from `seek` plus the document map, and `docs` on the node reports
`documents=0 docs_exact=0`, which means "not stored", not "no documents".

A document here is a **0x1E span**, not a shard record: MEMORY_OPS.md measures 6,979
internal 0x1E bytes inside document texts in the first 100 GB of the Pile, so span
ids split 0.043 % finer than documents. The number is a span count and is labelled
as such.

**No top-k truncation is applied: k = all children** (up to 257 including END). A
wide node's list is the exact histogram, which is what branch prediction needs; at
K=1000 the total edge mass is 2.7 M edges for 110 k nodes at 150 MB. Top-k plus an
exact remainder total is the next lever if a smaller ratio is needed at a lower
support; it is not applied today because it would make `branch-of` an approximate
answer.

## The descent, and the tail

`xsa relax <container> <context>` descends from the root as far as the context
exists and answers from the deepest node that does: descend while supported, back
off when not. This is the variable-order behaviour MEMORY_OPS.md asks for, not an
error path.

A context below support is an **empty answer with a flag**. It is not locate-scanned
and no synthetic fallback is invented: a locate costs ~40 ms per position on the
10 GB container (41.8 s for 1,000 positions), so the tail cannot be answered per
position by any locate-based path. The honest fallback for an exact answer on rare
text is `seek` (backward search over the container), and 98.6 % of 1024-byte
contexts have no repeat at all (CONTEXT_DEPTH.md), so an empty answer is also the
statistically expected one.

## Cost of the ordered stream (measured)

| step | 150 MB (n=1.5e8, r=5.7e7) | rate |
|---|---|---|
| dense run decode (32-64 threads) | 13.2 s at 10 GB (r=3.47e9) | ~260 M runs/s |
| LF walk through the query kernel | 878 s (12 threads, 4.4 us/step) | 0.17 M steps/s |
| LF walk through dense columns | 47 s (16 threads) | 3.2 M steps/s |
| the sweep itself (depth 8) | 28 s (1.5e8 rows) | ~190 ns/row |

The dense walk was verified byte-identical to the kernel walk on the 150 MB
fixture (`cmp` of the two 750 MB u40 suffix arrays: identical), and the sweep is
verified against brute force at every scale below.

## Gates

Each line below is the program's own output.

150 MB (`t150m.txt`, n = 149,998,882, container 128,207,384 B, member 9,317,624 B,
depth 8 / min_support 1000):

```
CONTEXT_TRIE depth=8 min_support=1000 nodes=94493 edges=2405363 bytes=9317624 ratio_vs_n=0.062118 docs_summary=exact<=8+hll13pct
CONTEXT_TRIE_COLUMNS node_fixed=377972 node_edge_delta=91608 node_count=208928 edge_code=2625224 edge_child=1392832 edge_count=3926168 doc_ndocs=32544 doc_top=13344 doc_first=220576 doc_ids=428232 head=192 bytes_per_node=98.6 bytes_per_edge=3.9
CONTEXT_TRIE_SOURCE mode=sa-cache-reuse rows=0 n=149998882 sa_bytes=749994410 dense_bytes=0 stream_mem_gb=0.7
CONTEXT_TRIE_BUDGET ratio_vs_n=0.062118 target<=0.05 pass=0
TRIE_VERIFY n=50 exact=50 mismatches=0 below_min_support=41
TRIE_VERIFY n=50 exact=50 mismatches=0 below_min_support=0
TRIE_BENCH samples=1000 ctx_len=16 depth=8 exact_depth_reached=0 branch_of_us_p50=7.96 p90=9.91 p99=15.32 max=1059.69 trie_stat_us_p50=1.29 p99=4.37 max=6.59
RELAX_STAT samples=1000 ctx_len=16 depth=8 reached_hist=0:0 1:10 2:70 3:135 4:183 5:183 6:147 7:104 8:168
RELAX_SUPPORT median_count=3770 p10_count=1271 us_p50=7.55 us_p99=12.98 us_max=1060.13
```

The second `TRIE_VERIFY` line is the one that can fail on counts: it samples 50
contexts **from the trie itself** (reconstructing each node's path from the parent
links) and compares the child list and every count against a brute-force scan of
the corpus. The first line samples 50 random 16-byte contexts from the corpus, 41 of
which are below support and therefore must be absent - which is what it checks.

### 10 GB (`/mnt/nvme2n1/erikg/pf-10gb.txt`, n = 10,000,000,001, chi 2,676,925,006)

Post-hoc build on the existing 10 GB container (`cs-gates/g3/pf-10gb.sxi2`),
depth 8 / min_support 1000, 32 threads:

```
DENSE_RUNS runs=3474883891 bytes=24351338263 wall=13.5s
SA_WALK rows=10000000001 steps=10000000001 wall=1100.3s dense=1
SA_CACHE_WRITTEN path=/mnt/nvme2n1/erikg/ctx-trie/pf10gb.sa5 rows=10000000001 bytes=50000000005
CONTEXT_TRIE depth=8 min_support=1000 nodes=3594261 edges=88185516 bytes=347726720 ratio_vs_n=0.034773 docs_summary=exact<=8+hll13pct
CONTEXT_TRIE_COLUMNS node_fixed=14377044 node_edge_delta=3482768 node_count=8403424 edge_code=96001552 edge_child=49728312 edge_count=148197352 doc_ndocs=1935928 doc_top=470288 doc_first=10700032 doc_ids=14429824 head=192 bytes_per_node=96.7 bytes_per_edge=3.9
CONTEXT_TRIE_SOURCE mode=dense-lf rows=10000000001 n=10000000001 sa_bytes=50000000005 dense_bytes=24351338263 stream_mem_gb=69.2
CONTEXT_TRIE_BUDGET ratio_vs_n=0.034773 target<=0.05 pass=1
CONTEXT_TRIE_BUILD rows=10000000001 sa_steps=10000000001 sa_wall=1192.7s doc_wall=3.9s sweep_wall=3448.1s total_wall=4696.2s threads=32 container_out=7352273840 member_off=347726720
TRIE_VERIFY n=50 exact=50 mismatches=0 below_min_support=19
TRIE_VERIFY n=50 exact=50 mismatches=0 below_min_support=0
```

The ratio is BETTER at 10 GB than at 150 MB (0.0348 against 0.0621): node count is
bounded by n / min_support, so it grows much more slowly than n. Wall 78 min
(1.2 min dense decode, 18 min LF walk, 57 min sweep); peak RSS 69 GB, which is the
5 B/position suffix array (50 GB) plus the dense run columns (24 GB).

Coverage and latency, 1,000 random contexts, one container open, no corpus read
(`branch-of --batch`):

```
BATCH_BENCH op=branch-of n=1002 p50_us=4.60 p90_us=6.90 p99_us=9.54 max_us=14.56 answered=610 below_support=392 corpus_touched=0
BATCH_DEPTH reached_depth_hist=0:2,1:0,2:11,3:32,4:61,5:92,6:99,7:98,8:607 mean_depth=6.944 asked_depth=8 trie_depth=8
BATCH_BENCH op=branch-of n=1005 p50_us=9.81 p90_us=14.35 p99_us=21.40 max_us=36.20 answered=615 below_support=390 corpus_touched=0
BATCH_DEPTH reached_depth_hist=0:0,1:2,2:8,3:29,4:64,5:72,6:93,7:127,8:610 mean_depth=7.013 asked_depth=16 trie_depth=8
```

**Coverage is 60.9 % of 8-byte contexts and 61.2 % of 16-byte contexts** (an
independent 1,000-context run by the operator measured 59.5 %, and the width-sample
prediction from the count distribution was 59.6 %: three independent methods agree
inside one point). The mean reached depth is 6.94-7.01 of 8. Timings above were
taken while another 32-thread build was running on the same box; a quiet-box run of
the same operation measured 3.84 us p50 (operator, 150 MB container).

### The whitespace point mass

The parent lane's 100 GB measurement (16 spaces = 762,642,500 occurrences) has its
10 GB analogue, and it is worth seeing in the trie directly:

```
$ xsa branch-of g10-d8.sxi2 "$(printf '        ')"
present count=126951032 depth=8 children=153
$ xsa trie-stat g10-d8.sxi2 "$(printf '        ')"
present count=126951032 depth=8 documents=3 docs_exact=1 top_doc=0 children=153
```

1.27 % of the 10 GB corpus sits on ONE depth-8 context, that context has 153
distinct children (all the bytes that follow a run of spaces), and **all 126 M
occurrences come from 3 of the 16 M documents** (the top one is span 0). Two
consequences, both structural rather than accidental:

- The node store is not distorted by this (one node, 153 edges), and the sweep's
  cost is per ROW, not per occurrence, so the point mass does not slow the build.
- The COUNT is not a document measure: a high support here means "one gigantic
  document", so `project`/`select` must weight by document frequency and the
  document map is load-bearing, not decorative. This is exactly the case MEMORY_OPS.md's
  `project` operation exists for.
- No top-k truncation is applied anywhere (k = all children), so the whitespace
  histogram is exact; if top-k is ever added it must keep an exact remainder total
  or the mass for "more spaces" becomes a lie.

### Why the 100 GB trie is NOT in this commit

The post-hoc path does not scale, and the numbers say where:

| component | 10 GB (measured) | 100 GB (projected) | Pile (projected) |
|---|---|---|---|
| u40 suffix array (5 B per POSITION, ignoring text) | 50.0 GB | 500 GB | 5 TB |
| dense run columns (7 B per RUN) | 24.4 GB | 220 GB | 2.2 TB |
| peak RSS of the build | 69 GB (measured) | ~720 GB | ~7 TB |
| LF walk (9.1 M steps/s at 32 threads) | 1100 s | 3.0 h | 30 h |
| sweep (one sequential pass, ~345 ns/row) | 3448 s | 9.6 h | 96 h |

So at 100 GB the reconstruction alone needs ~720 GB of RAM and ~13 h, and at the
Pile it is off the table entirely. **The primary path must be the construction
chain, not the LF cycle.** Where it goes, exactly:

- `xsa build`'s band producer (`cross_lcp_merge`) merges sorted suffix slices and
  therefore holds, per emitted row, the suffix position and the text. That is
  precisely the builder's input, and it costs one 32-byte text read per row that
  the merge is already making. A per-shard builder plus a K-way merge of the
  partial tries (a trie merges by summing counts and unioning children; the arcs a
  shard boundary cuts are already counted by the spans) removes the sequential
  sweep from the critical path: ~0.2 us/row spread over the shards.
- The finish's own serial fold (`xsa_finish.cpp`, the `sweep.advance(s.nbuf[i], ...)`
  stage) is NOT the place: it sees one RUN at a time (BWT char, length, head SA,
  tail SA), and the rows INSIDE a run have no SA value there, so it cannot read a
  suffix's first 32 bytes. That is the precise break in the one-ordered-pass plan.
- The container writer is `ContainerOut` in the same file (v7 streaming layout,
  trailing directory), which already appends members in stream order; m20 is one
  more section.

That work is not in this commit. No 100 GB trie number is claimed anywhere.

### The coverage/size dial, measured (10 GB, depth 8, 1,000 random 8-byte contexts)

Two full builds of the same corpus, same ordered stream, one flag apart:

```
COVERAGE depth=8 ms=1000 answered=610 below_support=392 p50_us=4.38 p99_us=9.12 ratio_vs_n=0.034773
COVERAGE depth=8 ms=100 answered=788 below_support=214 p50_us=4.53 p99_us=8.68 ratio_vs_n=0.165384
```

| min_support | nodes | edges | member | ratio_vs_n | answered | p50 | p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 3,594,261 | 88,185,516 | 347,726,720 B | 0.0348 | 60.9 % | 4.38 us | 9.12 us |
| 100 | 22,391,593 | 301,440,463 | 1,653,844,984 B | 0.1654 | 78.6 % | 4.53 us | 8.68 us |

**4.75x the bytes buys 17.7 points of coverage at the same latency** (the child
lists get no longer in any way that shows up: a binary search over 200 children is
one more comparison than over 20). That is a genuine trade, not a free lunch:

- `--min-support 1000` is the default because it is the only cell that meets the
  0.05-of-n budget (0.0348), and 61 % coverage is what the operation is worth at
  that size.
- `--min-support 100` is the coverage option: 79 % of random 8-byte contexts are
  answered exactly, at 0.165 of n - 16.5 GB on a 63 GB container at 100 GB scale.
  It FAILS the 0.05 budget and says so (`CONTEXT_TRIE_BUDGET ... pass=0`).

Note how the ratio moved with corpus size at a FIXED setting: 0.062 at 150 MB ->
0.0348 at 10 GB for min_support 1000, because node count is bounded by n/K and grows
more slowly than n. The 150 MB number is therefore an upper bound, not a projection.

## Residual risks

- The 100 GB gate is **not** met in this session: the dense columns for the Pile are
  226 GB, the u40 suffix array another 500 GB, and the sweep is a sequential
  1e11-row pass (~190 ns/row measured), i.e. ~10 h of machine time. The measured
  rates above are the projection's only basis; no 100 GB number is claimed here.
- 0.062 at the default is above the 0.05 target; the target is met at K=4000
  (0.0237) and at K=10000 (0.011). The `CONTEXT_TRIE_BUDGET` line exists so this
  cannot regress silently.
- Document ids are 0x1E spans, and above 8 distinct documents the count is an HLL
  estimate and the heavy document is an MG candidate.
- The dense run columns are build-time memory (5 B/run + 256 u64 per 1024 runs) and
  are not part of any container.
- `--sa-cache` is a 5 B/row file outside the container; it is a build accelerator,
  not a product.
