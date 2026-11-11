# Memory operations over a suffix tree — what we are building

This page exists because the project has drifted into naming structures instead of naming
*operations*. It states the goal, the smallest set of operations, what each needs, what is missing
today, and the gates. If a sentence here cannot be checked against a measurement, it does not belong.

## THE PRINCIPLE: document boundaries are metadata, never content bytes

At vocabulary 256 no in-band delimiter can be safe **by construction**: arbitrary binary payload can
contain any byte. The pile's `0x1E` framing is unique *by accident*, in one region, at current size —
and the accident decays with corpus size. Measured on the authoritative map
(`xsa corpus --pile-map`, `RESULTS_PILE_DOCMAP.md`, verified to the byte against the shard records):

| region | documents | internal `0x1E` inside document texts |
|---|---|---|
| full pile (1.31 TB) | 210,607,728 | **78,715 documents contain one** |
| first 100 GB (the spine container) | 16,119,766 | 6,979 internal bytes (0.043 %) |
| flagship train+eval (beyond 100 GB) | 243,401 | 0 (region-local; do not rely on it) |

Therefore:

1. **The index is the authority for structure.** Document starts live in the container as member
   **m15** (`xsa build --docs-map DIR`, or `xsa docstarts embed`; SXI2 v10): a monotone array with
   `rank`/`select` (delta-varint gaps + sampled absolutes, ~2.4 B/doc ≈ 0.04 % of corpus). Without
   the flag the build is byte-identical. `select(i)` is document `i`'s start; `rank(p)` is the
   document containing `p`; `p - select(rank(p))` is the depth in document — all O(log + stride).
2. **No query path scans for a boundary.** `xsa consult` cuts a position's spine at the document
   base (`max(0, off - base)`, explicit in the window or resolved from m15 by `doc`; REFUSED when
   neither is available), never at a separator. The removed `0x1E` spine cut was boundary inference
   from content bytes and was wrong on every document with an internal `0x1E`. `--doc-restrict`
   makes branch counts exact per document (a match never spans a boundary); `--exclude-source`
   removes the query position's own occurrence before any count is reported.
3. **What still counts `0x1E`, and why it is bounded:** the m20 context trie's per-node *span*
   summary counts `0x1E` spans (its doc map is built from the corpus bytes during the tape tap) —
   that is a node-annotation statistic, it is documented as a span count (never a document count),
   and it never feeds a consult row or a boundary decision. Re-pointing it at m15 is follow-up work.
   `--snap-1e` is an input-construction aid that REFUSES a mid-document raw cut — it never infers.

Requirement document: `NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md` (the emender's measured basis and
the three deliverables D1/D2/D3, gated in `RESULTS_STRUCTURAL_DOCS.md`).

## The goal, in one paragraph

A corpus index that a controller can *interrogate* while it writes or remembers: given the text I am
working on, which documents are the strongest match; what can come next in this exact context; how
much support is there; restrict all of that to a set of documents. Not retrieval bolted in front of a
model, and not an embedding approximation — the answers come from exact shared text. The unit is a
**context node**: a distinct string, its occurrences, the documents containing it, and the bytes that
follow it.

## Why a tree, and why not a fixed window

A 1024-byte window is a property of a *batch cache*, not of the data. Common text supports very long
contexts; novel text dies after a few bytes. The tree expresses that directly: descend while the
branch stays shared, ascend when support is thin. Depth becomes a measured property of each context
rather than a constant nobody can justify.

## The operations (from RESEARCH_MEMORY_OPS.md, cut to what is load-bearing)

| operation | question it answers | needs |
|---|---|---|
| `seek(s)` | where does this string occur, in which documents | suffix array interval + document map |
| `extend(node, b)` | what contexts follow this one | child edges (the tree) |
| `relax(node, min_support)` | ascend until the context is supported enough | tree parents + node mass |
| `project(node, docset)` | the same answer restricted to these documents | document map + occurrence/dc lists |
| `select(candidates, weight)` | rank by rarity-weighted match mass | document frequency per node |
| `matches(doc)` | the document's maximal exact matches against the corpus | tree + document map |
| `read(node)` | the stored text/continuation summary for a node | tree + corpus text |

Build order: `extend`, `relax`, `project`. `seek` exists today. `select` exists in embryo as the
rarity-weighted fanout; `matches` is the cats→domestication experiment; `read` is free once contexts
are addressable.

## What each operation needs from the container

| member | status | notes |
|---|---|---|
| suffix array as intervals (m1 BWT runs, m12/m13 rank directories) | **have** | rank arithmetic works; `xsa query --count` is exact on 10^11 bytes |
| sampled SA values (m11, 30.6 M) | **have** | enough to locate, not enough for tree topology |
| chi witness positions (m5, 24.19 G) | **have** | these are positions, NOT context nodes — a study corrected this |
| document map (offsets → document → component) | **have (m15)** | the shard-accounted starts, embedded as member m15 with rank/select (`xsa build --docs-map DIR`); boundaries are metadata, never a byte scan — see THE PRINCIPLE |
| **LCP over runs** | **in flight — the gate** | without it there is no tree: children are delimited by where LCP drops |
| child edges (explicit, for wide nodes only) | **not started** | children are contiguous in SA order, so narrow nodes can be enumerated by scanning LCP |
| stateless query core | **have** | read-only mmap, per-worker arenas, no shared cache; scaling measured (58 % at 96 threads) |

## What the data says about depth (CONTEXT_DEPTH.md, 1,000 sampled positions, 100 GB Pile)

The single most design-relevant measurement so far:

```
support at depth K        p50     p90     p99
K=2  (one repeat)         18 B    38 B   2082 B
K=10                      16 B    28 B    314 B
K=100                     12 B    22 B     96 B

at exactly 1024 bytes, positions with >=2 occurrences elsewhere: 1.4 %  (7.5 % counting the source)

repeating contexts:  depth 1  H=3.89 bits  top1=0.23   |   depth 8   H=1.29  top1=0.72
                     depth 16 H=0.50       top1=0.93   |   depth 32  H=0     top1=1.00
```

Three consequences, none of them optional:

1. The useful context length is ~8-16 bytes, not 1024. A 1024-byte window is a hundred times deeper
   than any signal in the data, which is why it is a cache artefact rather than a design choice.
2. Past ~32 bytes a repeating context is deterministic, i.e. it is boilerplate or duplicated text, and
   in the rare case where a long context does occur twice, answering from it returns the source
   document's own continuation - leakage, not information. Source-document exclusion must happen
   before any maximality or support test.
3. Cost is linear in depth and the information is not: `xsa branches` costs 472 us at depth 1 and
   51 ms at depth 1024. Going shallow is therefore both correct and ~60x cheaper, which is what makes
   the 150k positions/s target plausible at all.

Design consequence for the operations: `relax` thresholds belong around support 10-100 at depth
10-16; `extend` rarely needs to go past ~32 bytes; and document-level matching (`matches`, `project`,
`select`) carries the semantics, not context depth.

## What we measured, and what it says

- Build: 100 GB Pile in 12 h 35 m, container 0.632 n, scratch 0.789 n unbounded; with a scratch cap a
  10 GB build completes inside 6 GB and pauses/resumes correctly.
- Query correctness: exact against brute force on 10^11 bytes (counts, positions, a 192-offset
  discrepancy adjudicated in the index's favour — `grep -o` undercounts overlapping matches).
- Query speed today: 2,304 positions/s at 96 threads for a full next-byte histogram; 4,957/s for the
  bounded batch path. The consumer wants 150,000/s. The gap is per-position work (256 symbol lookups
  over decoded runs), not thread count, which is what the tree is for.
- Prior art: Infini-gram (2024) already does continuation queries over 10^12 tokens from suffix
  arrays; suffix-tree document clustering (1998) and context-tree weighting (1995) cover much of the
  theory. **Our claim is not novelty of continuations** — it is the operation set, the exact-match
  memory shaping, and the cost at this scale.

## The operation layer against the m20 API (measured reality, not intent)

`xsa/src/ctx.rs` materialises the top of the tree as member m20 and exposes `find(context) -> node`,
`children(node)` (the exact next-byte histogram, END included; pruned children keep their counts),
`count(node)`, `node_depth(node)` and `docs(node)`. The node store is 0.1-0.5 % of n because the depth
that carries information is 16-32 bytes, not 1024.

| operation | status over m20 | note |
|---|---|---|
| `extend(ctx, b)` | available | `find(ctx+b)`, or `children(ctx)` for the whole distribution |
| `branch-of(ctx)` | available | histogram is EXACT even for children too rare to materialise |
| `relax(ctx, k)` | available | no parent pointer is stored, but descending shorter prefixes costs <=32 finds |
| `select(cands, w)` | available | weight = count x 1/df; df exact for narrow nodes, HLL (~13 % SE) for wide ones |
| `project(ctx, docset)` | **not available** | m20 carries an exact document list only while a node has <=8 distinct documents; wider nodes carry exact top-1 plus an estimate |
| `matches(doc)` | available separately | `xsa mem` against the container |
| `read(node)` | available once a node is addressable | |

`project` is the one real gap. Cheapest exact route: `seek` + document-map lookup on the located
positions, reusing seps.bin/meta.bin. Alternatives are a document -> nodes inverted index, or exact
document lists for wide nodes (expensive).

## m20 is built IN-BUILD, from the sorter's band stream (measured 2026-11-10)

The trie used to be a POST-HOC pass: reconstruct the suffix array out of the finished container (one
LF cycle per row plus a 5 B/row column) and sweep it. That path is a scaling trap and it measured as
one at 10 GB: **1,192 s** to rebuild the ordered stream (47 GB of transient RAM, 69.2 GB with the
dense run columns) and **3,448 s** to sweep it single-threaded — 4,696 s and ~5 TB of RAM at the Pile
paid to decorate a build that already exists.

Inside `xsa build --tape` the ordered stream is already there. `scatter_sort --tape-bands` publishes
`band_<i>.sa` (8 B/row: position in the low 48 bits, carried BWT symbol in the top 8) and band indices
ascend in rank order, so `concat(band_0 .. band_k) == SA[0..row]`. The new phase `xsa ctx-tap` feeds
`ctx::Builder` from exactly that stream, concurrently with the sorter, the emitter and the finish:

- it waits for the emitter's `.opened` marker on band i (so no slice is removed out from under the
  emitter), reads the slice, **feeds the builder**, unlinks the slice and stamps `.consumed` — the
  marker the sorter's band window waits on. Under `TRIE_TAP=1` the emitter keeps the band files;
  with the variable unset nothing about it changes and the product is byte-identical;
- the per-row prefix read (D+1 bytes at a random corpus position) is issued by a worker pool and
  consumed in order by the single-threaded builder: the READS are parallel, the STATE is not;
- the member is joined to the finished container **in place** (the trailing directory is rewritten,
  no container byte is copied, 2.5 s at 10 GB), and the driver re-opens the result and journals
  `CONTEXT_TRIE_PUBLISHED` from the container rather than from the tap's claim.

Flags: `--trie-depth 8` and `--trie-min-support 1000` are the defaults; `--trie-min-support 100` is
0.165 n at 10 GB (0.3728 n at 150 MB) and answers ~77 % of random 8-byte contexts; `--no-trie` writes
the v7 container unchanged.

### Coverage and latency, measured on the in-build 10 GB container

`xsa branch-of <container> --batch ctx8.bin` with 1,000 random 8-byte contexts from the corpus
(xorshift64, seed 20261110), one container open, **no corpus read** (the batch file is the only input):

```
BATCH_BENCH op=branch-of n=1000 p50_us=4.70 p90_us=7.12 p99_us=10.26 max_us=14.99 \
  answered=588 below_support=412 corpus_touched=0 rss_gb=0.341 member_gb=0.324 build_stream_gb=0.00
COVERAGE depth=8 ms=1000 answered=588 below_support=412 p50_us=4.70 p99_us=10.26 ratio_vs_n=0.034773
BATCH_DEPTH reached_depth_hist=0:0,1:1,2:6,3:35,4:58,5:85,6:97,7:130,8:588 mean_depth=6.971 \
  asked_depth=8 trie_depth=8 warm_ms=4.4
```

The SAME batch against the post-hoc container: `answered=588 below_support=412 p50_us=4.96 p99_us=10.66`,
same per-context results, and `build_stream_gb=69.25` where the in-build member reads **0.00** — the
in-build route has no suffix-array column to charge. The two containers answer identically, which is the
functional half of the equivalence gate (the byte half is below).

### Why the feed must ride along (the ordered stream, named)

The builder is a sequential stack machine (row i's state depends on row i-1), and the sorter's bands
are produced by P parallel workers in arbitrary order, so the only place the rows are both in rank
order and out of the workers' hands is the band stream. What the tap does NOT do is reconstruct
anything: no LF cycle, no u40 column, no suffix-array cache, no text on disc. The band bytes it reads
are the bytes the emitter reads anyway (8 B/row).

### What it costs (10 GB, `pf-10gb.txt`, n = 10,000,000,001, threads 48, --memory-gb 24)

Two runs back to back on the same corpus, same flags, only `--no-trie` between them (box load average
8-17 during both, other jobs present; the arms ran sequentially, never concurrently):

| | `--no-trie` | trie (default) | delta |
|---|---|---|---|
| build wall (`/usr/bin/time`) | **2,032.07 s** (33:52) | **2,055.03 s** (34:15) | **+22.96 s = +1.13 %** |
| sorter wall (phase) | 31:50.78 | 32:05.49 | +14.7 s |
| sorter's own `SS_DONE total_s` | 1,902.93 s | 1,918.36 s | +15.4 s (+0.8 %) |
| emit-tape / finish-v2 wall | 31:49.8 / 33:35.0 | 32:04.7 / 33:50.4 | +15 s |
| ctx-tap wall | — | **32:06.4**, of which **work 943.3 s** (94 ns/row) and **wait 942.7 s** | — |
| sorter blocked on band credit | (n/a) | 4 diagnostic events (>=2 s each) in 1,925 s | ~0 |
| peak RSS (`VmHWM`) | 14,464,064 kB (13.79 GiB) | 14,320,316 kB (13.66 GiB) | **none added** (the two are within 0.9 % of each other and the trie arm is the LOWER one) |
| ctx-tap peak RSS | — | 10.0 GiB (= the read-only corpus mapping) | — |
| container | 7,004,547,080 B, v7, 8 members | 7,352,273,840 B, v9, 9 members | +347,726,720 B = m20 |
| container sha256 | `573d29135c2dbe49e784c75cb6613e5199263474bd65e1612bdc6c79b953513d` | `c242131e9d8efdb05f9b2b91f4f7c1df9c8f35d74713b8e82db58c9ac80bf4e6` | +m20 (and v9) |
| chi (both arms) | 2,676,925,006 | 2,676,925,006 | **unchanged** |
| member cost | — | 347,726,720 B = **0.034773 n** (nodes 3,594,261, edges 88,185,516) | — |

The wave-by-wave walls are the cleanest evidence, because the trie cannot influence them and they are
identical between the arms (waves 1..6, seconds):

```
--no-trie  24.79  122.66  61.41  90.89  95.26  85.26
trie       25.52  124.57  61.45  93.41  94.54  85.20
```

So the tap is **not** on the critical path: it did its 943 s of work inside a 1,925 s sorter wall and
spent the other 943 s waiting for the sorter to publish bands; the sorter's own log records only four
2-second band-credit stalls in the whole run. The trie costs the build **1.1 %** of its wall, where the
post-hoc route costs 4,696 s of separate work (1,192 s of LF reconstruction + 3,448 s of single-threaded
sweep) and 47-69 GB of transient RAM — and at the Pile those become five days and ~5 TB.

The feed's intrinsic floor is the single-threaded `Builder::feed` at ~94-108 ns/row (measured both in
the pipeline and standalone; the random prefix reads are parallel and cost ~8 ns/row once spread over
48 threads). That floor is 15-18 minutes at 10 GB, all of it overlapped.

### The same member, the other way, on the same day (`ctx-build` with a banked SA cache)

Measured after the two arms above, on the same corpus and container, with the 50 GB post-hoc suffix
array already on disc (`--sa-cache`, so the 1,192 s LF reconstruction is SKIPPED and this is the
post-hoc route at its best):

```
CONTEXT_TRIE depth=8 min_support=1000 nodes=3594261 edges=88185516 bytes=347726720 ratio_vs_n=0.034773
CONTEXT_TRIE_BUILD rows=10000000001 sa_steps=0 sa_wall=59.7s doc_wall=6.1s sweep_wall=3991.2s
  prefix_reads=10000000000 total_wall=4084.9s threads=32
G4_POSTHOC10_WALL 4095.26 s RSS 97831656 kB        <- 93.3 GiB peak, single-threaded sweep
```

so: **+22.96 s and +0 RSS in-build, versus 4,095 s and 93.3 GiB post-hoc** even when the 50 GB suffix
array is already paid for. That is the whole argument for the in-build route, in two lines of journal.

### Equivalence: the in-build member IS the post-hoc member

The member records which ordered stream produced it (head offsets 152..168: `source` and its memory
figure), so two routes that agree on every byte of the tree still differ in those 16 bytes BY DESIGN.
`xsa trie-member <container>` prints both digests: `sha256` and `sha256_noprov` (provenance zeroed).

- 150 MB (`/dev/shm/t150m.txt`): `sha256_noprov = f3d6a380...b77440` for BOTH the in-build member and
  `xsa ctx-build`'s post-hoc member from the same container; 5 differing bytes of 9,317,928, all
  inside the provenance field.
- 10 GB against the banked post-hoc member (`ctx-trie/g10-d8.sxi2`, 347,726,720 B, 3,594,261 nodes,
  88,185,516 edges): **7 differing bytes** of 347,726,720 - the 16-byte provenance field, and the
  member-format version byte, because that container was banked one commit before the format went to
  v2. `trie-info` on the two members prints the same depth/min_support/nodes/edges/bytes/ratio.
- the comparison is mechanical and checked in: `gates/trie_member_cmp.py same-code|legacy A.bin B.bin`
  zeroes ONLY the fields two routes are allowed to disagree on and prints the differing offsets
  (`same-code`: head 152..168; `legacy`: also the format version at 0..8 for containers banked before
  format v2), exiting non-zero if any byte outside them differs.
- 10 GB against a SAME-CODE post-hoc reference (`ctx-build --sa-cache` from the `--no-trie` container,
  `gates/trie_in_build_g4_tail.sh`): `sha256_noprov = 61fb82c5c7a218020d894ba5e56ff3cf6e5180733
  43187c36be2172f95e36c1e` for **both**, and **6 differing bytes of 347,726,720 — all inside the
  16-byte provenance field**. The two members are the same 3,594,261 nodes / 88,185,516 edges / 192
  head bytes to the byte.
- the ANSWERS are the same, not merely the bytes: `branch-of --batch` on 1,000 random 8-byte contexts
  returns **588 answered / 412 below support on BOTH containers**, with the same per-context results.

## Gates — no victory without all of these

1. `LCP_RUNS` member present at 150 MB, 10 GB and the 100 GB Pile head, with **50/50 sampled values
   verified against brute force from the corpus**. A wrong LCP builds a tree that answers wrongly
   while looking right.
2. Child enumeration at run granularity equals a brute-force next-byte histogram on ≥50 real
   contexts, at 10 GB and 100 GB.
3. Document map exact: `rank`/`select` from member m15 agrees with the authoritative map at every
   sampled position, INCLUDING positions inside documents that contain an internal `0x1E`
   (`xsa docstarts verify`; zero disagreements, 10k random + all internal-`0x1E` documents).
   The map is the shard record accounting — a document-count-equals-`0x1E`-count check is FORBIDDEN
   (it is off by 6,979 in the first 100 GB); the byte-range identity that holds is
   `separators == records + internal_0x1e`.
4. `extend`, `relax`, `project` implemented over the stateless core, with a descent measured per
   position; report positions/s/core and the node-depth distribution actually observed.
5. 10 GB and 100 GB indexes both rebuilt with the layer present, byte-identical answers for existing
   queries, and `origin/main` equal to local.

- **`0x1E` is NOT unique in the Pile.** Scanning `pile.txt` for the separator overcounts documents:
  in the first 100 GB it finds 16,126,745 boundaries against 16,119,766 records, i.e. **6,979 internal
  `0x1E` bytes inside document texts** (0.043 %). Document offsets must come from the shard record
  boundaries, and any check on a byte-range scan must use the identity
  `separators == records + internal_0x1e`, never a raw equality. The convention "0x1E is conventionally
  absent" holds for enwiki, not for arbitrary web text.
## Known traps (each one has already cost time)

- Two chi values are only comparable **on the same corpus**. Comparing across corpora produced a
  false "regression" and nearly deleted a load-bearing pass.
- χ is witness positions, `xsa mem` extends on the *query's* byte, and m7 is a byte permutation —
  not context nodes, not index-side extension, not LCP.
- A generic "quiet for N minutes" watchdog kills healthy builds; the census is silent for ~35 min at
  the pile. Health checks must measure journal freshness AND CPU AND scratch deltas (`HEARTBEAT.md`).
- Container members cannot be appended in the old layout; streaming is a format change (SXI2 v7) and
  the legacy layout stays readable.
