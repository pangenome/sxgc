# Can chi supply right transitions?

Measured 2026-11-09/10 UTC. Study C, based on `origin/main` **8939896bfea62effa17e7467dadb5cdd852afdf0**;
product sources unchanged. Sizes below are decimal GB/TB. The 100 GB container
was opened read-only, not rebuilt.

**No: m5 does not enumerate distinct contexts or provide child steps.** It stores
suffixient witness *text positions*. A witness can cover many requirements;
its ordinal is not a context ID. The premise “24.19 G chi = exactly the ladder's
node set” is false. A new transition structure is possible, but its state count,
edge labels, state mapping, counts, and navigation must all be constructed.
The present members do not make that construction a cheap scan of chi.

Recommendation: reject a chi-sized transition-table implementation based on that
premise. A reversed-corpus companion gives the required direction by construction;
qualify an optimized reversed rank/histogram kernel before spending the projected
week / 830 GB. The current rank kernel and the anchor hybrid do **not demonstrate**
150,000 positions/s on this node. A tiny hot transition-table proxy is fast, but
is not a working chi-indexed right-extension implementation.

## Evidence and source provenance

Read the root `NOTE_FROM_EMENDER_EXTRACTION_PERFORMANCE.md`, including its
ADDENDUM; `RESUME_DESIGN.md`; and `bit6/SXI_FORMAT.md`. The requested
`CHI_STUDY.md` is not present on the pinned origin/main. Read the existing study
from `/home/erikg/chi-study/tools/binbwt/CHI_STUDY.md`, committed at
`222ace4044cdaaf9e3ad8710d954afc5493c12bb` (document SHA-256
`fd2d7e28b62ae34a22606c2ca47dc2fa49d053541f5b40d9a248a52a14e613d1`).
In particular its sections 1 and 3 explicitly distinguish witnesses, requirements,
and context identities. This study does not import its older implementation costs.

The actual file is `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2`:
100,000,000,001 rows including virtual END; 31,381,597,694 runs;
24,185,339,204 chi positions; **63,163,489,696 bytes**.

| Member | Actual payload bytes | What it supplies; can it append b? |
|---|---:|---|
| m1, codec 114 | 29,738,280,736 | C table and blocked Huffman/gamma RLBWT. `rank(b,r)-rank(b,l)` counts **preceding** bytes. Supports prepend via LF/rank, not constant-cost append. |
| m4, codec 4 | 12 | XANC header, zero record anchors. Not chi, no transition map. |
| m5, codec 105 | 12,194,502,226 | Elias–Fano sorted witness coordinates. No context strings, depths, suffix intervals, parent links, child links, or branch counts. |
| m7, codec 7 | 256 | Byte permutation, **not LCP**. The producer's LCP bands are transient inputs, not a persisted suffix-tree navigation member. |
| m10, codec 110 | 0 | Declared LF-start member; values are derived using C and m13. No forward links or sampled ISA. |
| m11, codec 111 | 245,168,752 | Raw SA at tails of runs 0,1024,...; 30,646,092 anchors. Resolves a row by walking LF until an anchored tail. Run sampling gives no 1024 bound on text-distance walks. |
| m12, codec 112 | 248,999,568 | Two-level row-to-run/block directory, accelerates BWT row access. No string child relation. |
| m13, codec 115 | 20,736,537,752 | Per-symbol cumulative-row/run samples plus second-level directories, accelerates rank and LF. No next-byte counts at context depth. |

Source anchors: `xsa/src/qlite.rs` (`rank`, `search`, `locate_steps`, `cum_before`,
`row_run`); `xsa/src/branches.inc.rs` (`extension_counts`, `branch_result`);
`xsa/runtime/bit6/xsa_finish.cpp` (small-member serialization near line 6805,
`SweepImpl`, witness emission); `xsa/src/sxi2.rs` (chi EF decoder);
`lean/Sxgc.lean` (`suffixient`, `chi`, requirement definitions).
The format version matters: old SXI2 versions materialized LF starts; this v6
container has none. m4 and m7 retain their SXI1 meanings.

Neither a constant-cost parent step in a suffix tree nor arbitrary forward
navigation is serialized here. Backward search is a **left extension**, not
necessarily a parent step. Existing right-branch serving already works without
a corpus pass by separately searching `context+b` for each present byte; that
is exact, but does not reuse the context's interval as a child transition.

## Measurement method

Prototype files are under `xsa/tests/chi_transitions/`. `run.py` copies sources
into a temporary crate, disables the unused native-tool bundle in that copy,
and appends `probe.rs` to the private qlite module. It uses the actual production
rank, search, locate and extension-count code; no product code or format changes.
The runner builds optimized Rust and deletes its temporary source/binary on exit.

Command (repository root):

```sh
python3 xsa/tests/chi_transitions/run.py \
  /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 \
  /mnt/nvme2n1/erikg/pile-head100.txt 256 \
  > /tmp/chi-transitions.csv 2> /tmp/chi-transitions.log
python3 xsa/tests/chi_transitions/run.py \
  /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 \
  /mnt/nvme2n1/erikg/pile-head100.txt 128 ladder \
  > /tmp/chi-ladder.csv 2> /tmp/chi-ladder.log
python3 xsa/tests/chi_transitions/summarize.py \
  /tmp/chi-transitions.csv /tmp/chi-ladder.csv
```

256 deterministic xorshift-selected real text contexts, 64 each of lengths
8, 32, 128 and 1024, seed `0x932ab572ac384ecd`. All occur in the corpus.
These are occurrence-weighted samples, **not uniformly sampled trie nodes or
consumer document positions**. The final CSVs and JSON summary are checked in
alongside the probe. Percentiles use nearest rank, independently for each metric;
combined percentiles are computed per sample before sorting.

Rust 1.97.1 (`8bab26f4f`), Cargo 1.97.1; optimized release build.
One process, one thread; AMD EPYC 7713, two sockets, 128 physical cores / 256 SMT
threads. Shared-node load was about 80–101 during measurement; no cache flush,
CPU isolation, or frequency control. Startup and compilation are excluded.
The 64 MB decoded-block cache persists across samples; random storage faults,
scheduling, and mmap faults during each operation are included.

The operations are deliberately separated:

1. **Reversed-index primitive proxy:** on each actual forward interval, compute
   243 present-byte rank differences (486 rank calls), plus 1024 repetitions of
   a nonempty interval extension primitive (2048 rank calls). Histogram here is
   the *preceding* distribution. The same operations on a reversed-corpus
   interval would answer next bytes. This is a timing surrogate, **not a built
   reversed index or a measured real rolling ladder**. Repeated identical rank
   addresses make the ladder portion unusually hot; a real ladder has different
   intervals, some empty. Neither bias can be extrapolated into a node SLA.
2. **Actual maintained ladder, companion orientation surrogate:** a second run
   feeds bytes backwards from original-text offset 50,000,001,000. Ramp 1024
   positions, then measure 128 consecutive positions, updating 1024 distinct
   ladder states in descending length order plus 243 histogram rank differences.
   This is a real maintained ladder using the existing index as the companion
   of the *reversed* text; it does not build the companion of the forward text.
   Three full-length intervals are independently checked by fresh search, and
   all histogram sums equal interval width. All 128 full-length intervals have
   width 1. It has 2534 rank calls/position (2048 ladder + 486 histogram).
   These measurements, rather than the repeated-interval proxy, appear in the
   comparison table. Reversal can change BWT runs/locality, so a production
   reversed-corpus speed claim still requires the actual companion.
3. **Transition-table hot proxy:** 1024 sparse child-label lookups in an expanding
   in-memory pool of the sampled exact histograms, then a summary reference.
   This pool has at most 256 rows. There are no global context-to-ID lookups,
   actual child IDs, disk faults, or multi-terabyte state. It measures lookup
   mechanics only, not the existence or cost of a chi-based implementation.
4. **Actual anchor hybrid:** fresh backward search of each context, locate
   **every** row when interval width <=64, fetch `corpus[SA+length]`, and compare
   all 256 counts with independent exact rank-based extension counts. Broader
   intervals are marked -1/skipped, never truncated. Original text is necessary
   for this route. Pre-sampled queries exclude EOF-crossing cases; this is not
   an END/boundary acceptance gate.

The exact forward-rank search for all `context+b` is also timed, to show the
working zero-corpus-pass fallback. No memory mapping is confused with an
in-memory index: anchor lookups and subsequent LF walks can fault storage.

## Three-route comparison

| Route | Added persistent disc, 100 GB / pile | Build cost | Measured position p50 / p99 (us) | 150,000/s on this node? |
|---|---|---|---:|---|
| Reversed companion: real ladder + rank histogram, orientation surrogate, 128 positions | ~63 GB / ~830 GB (projected) | Second full build; forward receipt 12h35m at 100 GB; pile ~week estimate | 18,523.766 / 29,087.131 | Not demonstrated; proxy mean 57.5/s/core, ideal 128-core ceiling 7,362/s |
| Chi transition layer: hot lookup mechanics only, 256 contexts | Invalid K=chi assumption: varint tree ~54 GB / ~713 GB; fixed-width tree ~151 GB / ~2.30 TB; actual K unknown | New state/edge census and construction; unmeasured | 30.980 / 50.260 | Mechanics fit CPU budget; no functioning chi route or capacity-scale result |
| Anchors + LF + original text: exact, 194 narrow contexts | 0 if text retained already; otherwise +100 GB / ~1.32 TB | No new index build; retain text | 335,909.660 / 19,152,709.997 | No on measured workload; mean 0.824/s/core, ideal 128-core ceiling 106/s |

The 150,000/s target permits **6.667 us/position on one core**, or **853.333
us/core/position if perfectly spread across 128 physical cores** (1171.875
positions/s per core). SMT is not another 128 independent cores. Multiplying a
single-thread rate by 128 is an optimistic CPU-only ceiling, not a measurement
of socket scaling, shared cache, NVMe contention, IPC or scalar assembly.

The addendum's 1–3 us per position assumes just 1–3 ns for each of 1024 complete
steps. An FM step needs two ranks, and a large transition table needs real
memory accesses. Even granting that lookup latency, the claimed rate is not
established by the ladder recurrence. Empty states can short-circuit; that
requires consumer-trace measurements rather than silently counting all 1024
rungs as nonempty or free.

## Size models: hypothetical chi nodes versus actual contexts

A usable state must encode interval **and depth** (the same interval can represent
many unary contexts), or an equivalent edge/depth state. A child histogram alone
does not supply the child state's identity. A 256-bit mask plus counts omits
navigation unless child IDs are implicit in a newly built topology.

Let K be the **actual** number of stored states, E their outgoing edges, and
d=E/K. The following are optimistic fixed-width payload models: at 100 GB,
count/position fields need 37 bits and chi-sized IDs 35 bits; at projected pile
n≈1.32e12 and chi≈317e9 they need 41 and 39 bits. Byte-packed counts/IDs thus
use 5/5 and 6/5 bytes respectively. Pool pointers use 5/6 bytes **if their
address range fits**; larger edge pools require another byte. No allocation,
indexing, CRC, alignment, EOF, node identity or cache-directory overhead is priced.

| Representation | Bytes/state at 100 GB | Bytes/state at pile |
|---|---:|---:|
| Sparse labels + child IDs + counts + pool offset | 5 + 11d | 6 + 12d |
| 256-bit mask + pool pointer + child IDs + counts | 37 + 10d | 38 + 11d |
| Mask + pointer + counts ONLY (not navigable by itself) | 37 + 5d | 38 + 6d |
| Level-ordered tree: implicit IDs, edge label, count, LOUDS | about 6.25 (tree E≈K) | about 7.25 (tree E≈K) |

For the final row the 2 bits/state topology excludes select/rank overhead and
state-to-context mapping. It assumes a real tree with each child stored in BFS
order. This does not give suffix links or solve online state/depth/label access.
For a graph with shared states use measured E/K, not the tree formula.

| Hypothetical K=chi payload | 100 GB, chi=24.185339204 G | Pile, chi=317 G |
|---|---:|---:|
| Sparse d=1 / 2 / 4 | 386.97 / 653.00 / 1185.08 GB | 5.706 / 9.510 / 17.118 TB |
| Mask+navigation d=1 / 2 / 4 | 1136.71 / 1378.56 / 1862.27 GB | 15.533 / 19.020 / 25.994 TB |
| Mask+counts only d=1 (incomplete) | 1015.78 GB | 13.948 TB |
| Fixed-width level-order tree | 151.16 GB | 2.298 TB |
| Level-order tree, 1-byte varint count/edge | 54.42 GB | 0.713 TB |
| Labels+LOUDS only (counts omitted/implied) | 30.23 GB | 0.396 TB |
| Existing container / proposed reversed companion | 63.16 / about 63 GB | about 830 / about 830 GB |

These are **conditional arithmetic, not estimates that K=chi is correct**. Under that unsupported assumption, the fixed-width tree is about 2.4–2.8
containers and the sparse unary representation about 6–7 containers. Varint
counts (1–6 bytes each) could put the tree in the **same size class** as the
container: 2.25 bytes/state when every count fits one byte, before rank/select
and context mapping. Counts shared along unary edges could reduce it further.
Thus there is no information-theoretic claim that every compact transition
structure exceeds the companion. The missing state census and navigation are
the issue. Masks alone cost 773.93 GB / 10.144 TB.

An explicit trie containing **every context of length <=1024** has
`D = sum_i (min(1024, suffix_length_i) - min(1024, LCP_i))` nodes (excluding root,
with document/END rules applied). D can approach 1024n, not chi. A unique
128-byte string at a position implies unique longer strings at that same start;
these create hundreds of distinct unary depths. Unary path compression can
avoid materializing them, but requires edge labels, access to bytes along edges,
and edge/depth states. A suffix tree/automaton can change this model; m5 is
neither of those serialized structures. No chi-sized construction theorem or
implementation is supplied by the present format.

Measured branch-count distribution (full histograms, including counts for every
present byte; no top-k truncation):

| Context length | Samples | Mean outgoing degree | Degree=1 samples | Interval width p50 / p99 | Gathered samples |
|---|---:|---:|---:|---:|---:|
| 8 | 64 | 37.515625 | 8 | 32,563 / 23,608,214 | 7 |
| 32 | 64 | 1.3125 | 62 | 1 / 446,494 | 59 |
| 128 | 64 | 1 | 64 | 1 / 3 | 64 |
| 1024 | 64 | 1 | 64 | 1 / 2 | 64 |

Overall 198/256 histograms have one outgoing byte; maximum degree is 132.
The complete degree histogram is in `summary.json`. Repeated occurrences may
share the same next byte, so degree=1 is **not** equivalent to interval width=1.
The large number of unary long contexts supports considering path compression,
but does not convert suffixient witnesses into context nodes.

These sampled degrees cannot estimate node-weighted E/K: long, unique contexts
are sampled frequently by position but collapse on compressed unary edges;
right-maximal nodes are an entirely different population. The true distinct
context count and global branch distribution cannot be decoded from m5.
A full bounded-LCP census would be needed, and the current container does not
retain that LCP stream. The table therefore gives d sensitivity instead of
presenting a biased sample mean as a global fanout estimate.

## Hybrid: why sparse anchors do not recover forward bytes cheaply

Backward search costs at most 2m ranks (m<=1024), stopping early if absent.
For a present interval of width h, gather costs **h anchor lookups plus the sum
of h LF distances**, and h random corpus-byte reads. Each LF step needs
row-to-run lookup and the run symbol's cumulative rank (m12/m13 + decoded m1).
The anchor checks are at run tails, not arbitrary rows. Average anchor density
here is n/30,646,092 = about 3263 text rows, but this is **not** an expected
walk distance theorem or a worst-case bound. Measured walks are reported below.

194 contexts / **395 anchor lookups** passed exact histogram parity against
independent forward-rank extension searches. Total LF steps **2,615,648**;
mean **6621.894 steps per located row**. Per-context sum of LF steps p50/p99 is
**2429 / 325,487**. Gather-only time p50/p99 is **297,729.716 / 19,146,090.528 us**;
search plus gather is in the comparison table. The other 62 contexts were too
broad for this gather experiment; production cost would increase with h.

For all 256 contexts, fresh context search p50/p99 is **11,091.156 /
220,385.966 us**. Exact existing `context+b` searches, excluding that preliminary
context search, take **24,370.485 / 131,688.968 us** p50/p99. Those operations
are measured on this bounded-cache kernel and shared machine; they must not be
confused with the different 22,768 contexts/s service receipt in the request.
The companion surrogate's maintained-ladder portion alone takes **1517.363 /
3113.595 us** p50/p99, and histogram ranks **16,459.484 / 27,130.898 us**.
Even its mean ladder time alone (1618.865 us) exceeds the 853.333 us physical-core
budget for all-present contexts at perfect 128-core scaling. Different kernels,
empty-rung pruning and consumer distributions could change that conclusion.

Without the source corpus, SA coordinates alone do not provide `T[SA+m]`.
There is no sampled ISA or text-byte array. One could implement inverse-LF/Psi
using BWT select and advance m+1 times from each suffix row, but native qlite
has no such constant-cost select/forward-text accessor. LF moves to the preceding
text position, not the following one. Calling those walks “short” is unsupported.
Retaining the original corpus costs an additional **100 GB / about 1.32 TB**
if it would otherwise be deleted, exceeding a companion index's disc at pile.
Our benchmark uses the already-existing corpus, so makes no sequential corpus
pass and incurs only sampled/random reads.

## Build costs and decision

The banked `P100c/{order-tape,emit-tape,finish-v2}.time` receipts report overlapping
producer walls 12:21:46 / 12:21:42 and finish 12:35:10. These are historical
forward-build measurements, not measurements of reversing a corpus or building
a companion. The prompt's approximately one week / +830 GB at pile remains a
planning estimate; reversed text scratch, build scratch, and a reverse-output
pass are additional transient costs. Reversal can change R and output size.
A companion is a second full index build, but no locate or corpus lookup is
needed at consultation time.

A transition structure requires at least enumerating its states/edges and writing
its output. The hypothetical fixed-width 151 GB / 2.3 TB tree is larger than the
companion; the varint tree could be comparable if K really were chi. Neither
is a demonstrated construction or a universal storage lower bound. If constructed
alongside a future suffix/LCP stream it could share a build traversal; from this
existing file it needs a new derivation (LCP reconstruction, source/index traversal,
or rebuild), not merely m5 decoding. No honest wall-time estimate follows from
chi cardinality. The hybrid needs no new index build but its random-walk query
cost and retained text are unacceptable here.

Proceed with reversed-index kernel qualification on a small real reversed
fixture and representative consumer traces, including absent contexts and full
histogram parity. Optimize combined BWT histogram access rather than assuming
243 independent ranks are mandatory. Consider compressed suffix-tree/automaton
navigation only as a separate design with a measured node census and byte-access
plan. Do not commit a week-long pile build based on a 1–3 us ladder estimate.

## Residual risks

- The forward corpus's actual reversed companion and a full-size transition structure
  were **not built or benchmarked**. A real maintained ladder was measured on
  reverse-fed source bytes using the original container as an orientation surrogate.
  Production performance must not be inferred from these proxies.
- No consumer-trace distribution, document separators, EOF oracle, all-absent
  ladder optimization, NUMA/multicore scaling, IPC, or feature-assembly qualification.
- Branch sampling is position weighted; no full LCP or distinct-node census.
- Shared-node storage/load affect tails; warm tiny tables cannot predict an
  out-of-core transition index. No second full corpus pass or product changes.
- Supervisor/intercom tools were absent from the available tool catalog, including
  a search for both names/descriptions; no supervisor coordination was possible.
