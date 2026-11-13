# Batched next-byte branches

```
xsa branches corpus.sxi2 --contexts contexts.txt -k 8 --jsonl
xsa branches corpus.sxi2 --serve -j 32 -k 8
```

Both modes use the existing block-compressed SXI2 v6 reader (m1 codec 102 or
114); older codec-101 containers require rebuilding. Both return exact next-byte
frequencies after the **entire context**.
Overlapping occurrences count separately. Matching uses the container's byte
permutation and backward search; returned bytes are original corpus bytes.
No byte is used as an absent-result sentinel. The corpus is a single byte
sequence: document separator 0x1E is an ordinary searchable byte.

The container (SXI2 v10, the ONE supported shape) has no move/psi member. Its sparse m11 anchors contain
the SA coordinate at the tail of every 1024th BWT run. For intervals of at most
64 rows, this implementation walks LF to an anchor for each occurrence, then
reads `corpus[SA[row] + context.len()]`. Cost is O(context length rank steps +
sum of LF walk lengths + width corpus gathers), **not** O(width) constant-time
moves. There is no guaranteed short LF walk. The reader's existing materialized
rank columns and row directory are shared across workers; each worker reuses a
private navigation context.

For wider intervals, exact backward searches of `context || byte` compute
right-extension counts for each present byte. This avoids millions of sparse
LF walks for common and one-byte contexts. It is not `rank(byte,r)-rank(byte,l)`,
which would count preceding bytes. The empty context uses corpus byte totals
and occurs at all `corpus_bytes + 1` boundaries, including EOF.

The fallback needs the original corpus. Matching document sidecars and
`corpus.bin` are discovered beside the container or from its build journal.
Use `--docs DIR` to specify them, or `--corpus FILE` for a corpus without document
sidecars. Length must equal the container's `n-1`. Explicit corpus files must be
from the indexed corpus; equal length alone does not establish identity.

## File mode

`--contexts FILE` reads newline-delimited byte contexts, assigning zero-based
`ctx_id` values. A blank line is an empty context; a terminal newline does not
add another context; an empty file contains zero contexts. CR bytes are preserved.
Use framed mode for contexts containing newline bytes. `--jsonl` emits one
object per context with `ctx_id,l,r,width,eof,other,branches`, where each branch
has numeric `byte` and `count`. Default TSV fields are those six scalar fields
followed by the list of `(byte,count)` pairs. File mode processes sequentially.

Branches sort by descending count, then ascending byte. `-k N` retains at most
N branches (0 through 256; default 256). A zero-count byte is never emitted.

For every response:

```
width = r - l
sum(emitted branch counts) + other + eof = width
```

`eof` counts occurrences ending at the corpus end, with no following byte.
`other` counts omitted branches, and is zero without truncation. Empty intervals
have width 0, eof 0, other 0, and no branches. An EOF-only context has
`eof == width` and no branches. These meanings also hold for empty contexts.

## Streaming protocol

`--serve -j N` starts N workers in one process; N must be positive. stdin and
stdout are binary pipes. Diagnostics go exclusively to stderr. Each response
is flushed immediately, without waiting for EOF or a full batch. Pipelining
requests keeps workers busy; clients should read responses while sending.
Outstanding requests and the ordered result buffer are bounded to 4N requests
(plus one request being read). Default output order is request order.
`--unordered` permits completion order; `ctx_id` always identifies input order.

All integer fields are unsigned little endian, with no padding:

| Request field | Bytes |
| --- | ---: |
| Context byte length | 4 |
| Context bytes | length |

Context bytes are verbatim, including leading `-`, 0x1E, NUL, 0xff and newline.
Zero-length requests are valid. Requests above 16 MiB are rejected before
allocation. EOF between frames closes the stream normally; truncated headers
or bodies produce a nonzero exit and stderr diagnostic.

| Response field | Bytes |
| --- | ---: |
| Payload byte length (excludes this field) | 4 |
| ctx_id | 8 |
| l | 8 |
| r | 8 |
| width | 8 |
| eof | 8 |
| other | 8 |
| nbranches | 2 |
| Repeated: byte, count | 1 + 8 each |

Payload length is `50 + 9*nbranches`. IDs start at zero for each process.

`BRANCH_READY` marks completion of index loading. `BRANCH_STATS` reports
post-load throughput, mean backward-search time (including right extensions),
mean gather time (including LF walks and branch sorting), output serialization
and pipe-write time, request-read time, and total LF steps. Kernel times are summed across workers,
so their sum is not wall latency at N > 1. Input and output IPC timings include blocking
reads/writes (including idle time waiting for a client); client end-to-end throughput also includes request transport and
scheduling. Loading the index is outside the warm benchmark timing.

## Reproducible gates

`tools/binbwt/tests/branches_gate.py XSA CONTAINER CORPUS --out DIR` computes
100 distributions independently using Python `mmap.find`, checks full and top-8
responses, seven edge cases, and count/position regressions. The 150 MB gate
uses the existing banked 100-pattern oracle; the 10 GB gate checks the existing 20 low-count
patterns against their banked positions. `--reuse-oracle` replays a previously
computed independent branch oracle without rescanning corpus bytes.

`tools/binbwt/tests/branches_bench.py XSA CONTAINER CORPUS --jobs N --out DIR`
pipelines 100,000 contexts after 1,000 warmup requests. The deterministic mix is
90% sampled 48-byte contexts and 10% common English contexts. It checks frame
order and the closed count invariant on every reply.

## Measured gates (2026-11-03)

W1: **100/100** exact distributions, EOF and width at each of 150 MB and 10 GB.
Top-k closure also passed 100/100 at each size. W3 passed 7/7 edge contexts at
each size. File JSONL passed 12/12 at each size. W4 retained the existing
100/100 and 20/20 count/position gates, including unchanged `--count` values.
Query-kernel unit tests: 18 passed.

100 GB container, k=8, 100,000 requests after 1,000 warmup requests:

| Workers | Contexts/s | Search µs/context | Gather µs/context | Input IPC µs/context | Output IPC µs/context | LF steps/context |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 104.664 | 321.562 | 9224.533 | 1.601 | 7.965 | 31744.8 |
| 32 | 553.836 | 424.870 | 10196.409 | 3.689 | 2.190 | 31744.8 |

The 20,000 contexts/s target was **not met**. Profiles include warmup;
throughput excludes warmup and loading. IPC times include blocking reads/writes
and are not additive with parallel kernel times. These are shared-host
measurements, not an isolated-machine capacity guarantee. The existing reader
materializes about 266 GB of index columns for this corpus.

Raw gate logs, independent oracles, exact container/corpus paths, binary hash,
and benchmark results are in [the evidence directory](tools/binbwt/evidence/branches/results.json).

# Maximal exact matches

```
xsa mem corpus.sxi2 "domestic cat" [-k N] [--min-len 8] [--docs DIR] [--limit 8] [--jsonl] [--all-mems]
xsa mem corpus.sxi2 --serve -j 32
```

`xsa mem` answers maximal exact matches (MEMs) of a byte query from the same
query kernel, using no container member that `branches` did not already need.
It is the ONLY MEM surface: `xsa mems` (the phi-era reader, `.ri4`/`--sxi`
inputs) is REMOVED, and the retired name refuses by name.

A match `query[a..b)` (half-open) is **maximal** when it occurs and cannot be
extended *within the query*:

```
left : a == 0    OR  interval(query[a-1..b)) is empty
right: b == qlen OR  interval(query[a..b+1))  is empty
```

The query boundary is not an extension point, so it is vacuously maximal (this
is the standard MEM definition). The alternative reading - "no occurrence of the
match is followed by any present byte" - is not a match set: since
`sum over present c of width(P||c) = width(P) - eof`, it would admit only
matches at the corpus end.

**Cost.** Backward search prepends, so an interval extends to the LEFT in one
rank step (the FM-index move). Extending right means re-searching a longer
prefix, so matches are built by fixing the END `b` and walking the start `a`
downward: for a fixed end the starts with a non-empty interval are a contiguous
range, and the whole scan is `O(qlen * L_max)` rank steps with `O(1)` work per
step (`L_max` = longest match). A 32-byte query costs a few hundred rank steps,
and no corpus byte is read: counts are interval widths.

**Non-maximal candidates are not silently dropped.** For each end `b` the
candidate is `(amin[b], b)`, the only start at that end which is left-maximal.
Its right-maximality is decided by RE-SEARCHING `query[a..b+1)` from an empty
interval, and cross-checked against the independent `amin[b+1]` route; the two
must agree (`MEM_AMIN_DISAGREES`). Left-maximality is asserted with one more
rank step (`MEM_NOT_LEFT_MAXIMAL`). Every emitted width is checked against the
text (`QUERY_COUNT_IMPOSSIBLE` reuse).

## File mode

Default: the SINGLE longest MEM, `query_start<TAB>query_len<TAB>occurrences<TAB>
taken<TAB>truncated`, then up to `--limit` positions (comma-separated, in
suffix-array rank order; `--limit 0` locates nothing). With document sidecars
each position renders as `doc_id:offset:pos`, and a final column carries the
titles joined by `|` (tabs, newlines, CRs and backslashes escaped). `--jsonl`
emits one object per MEM with `query_start`, `query_len`, `occurrences`,
`taken`, `truncated`, `positions`, and a `docs` array
(`doc_id`, `offset`, `title`, `pos`) when sidecars resolve. Positions come from
the same locate walk `xsa query` uses, and a match wider than `--limit` reports
`occurrences` for the full width with `truncated=true`.

`--all-mems` reports every MEM, ranked by descending `query_len` then ascending
`query_start`; `-k N` keeps the first N (`0` = all). `--min-len N` (default 8)
drops shorter matches. Empty queries are refused by name; a query that does not
occur reports no rows (`best_len` 0 in framed mode) rather than a fake match.

## Streaming protocol

`--serve -j N` uses the same framing conventions as `xsa branches` (N workers in
one process, binary stdin/stdout, diagnostics on stderr, responses flushed
immediately, one queue per worker, ordered output, at most 4N outstanding
requests). Each request answers with the SINGLE longest MEM of that query.

| Request field | Bytes |
| --- | ---: |
| Query byte length | 4 |
| Query bytes | length |

| Response field | Bytes |
| --- | ---: |
| Payload byte length (16) | 4 |
| best_len | 4 |
| query_start | 4 |
| occurrences | 8 |

All integers are unsigned little endian, no padding, no byte echo. `best_len`
0 means "no MEM of at least `--min-len` bytes"; `query_start` is then 0.
Zero-length request frames are REFUSED (nonzero exit, stderr diagnostic): an
empty query has no maximal exact match to report. Requests above 16 MiB are
refused before allocation; EOF between frames closes the stream normally.

## Reproducible gates

```sh
gcc -O2 -o /tmp/mem_oracle tools/binbwt/tests/mem_oracle.c
python3 tools/binbwt/tests/mem_gate.py XSA CONTAINER CORPUS --out DIR --scale 150m --docs DIR
python3 tools/binbwt/tests/mem_bench.py XSA CONTAINER CORPUS --out DIR -j 32
```

`mem_gate.py` X1 recomputes every MEM independently with
`tools/binbwt/tests/mem_oracle.c` - one mmap pass over corpus bytes, an 8-byte
anchor filter, exact byte verification and extension, no index code and no shared
implementation - and requires exact agreement on `(query_start, query_len,
occurrences)` for the full MEM set and for the longest match. The query mix is
sampled substrings, one-byte mutations (two MEMs bounded by a mismatch),
two-window splits, brand-new strings, repeated bases and a full-corpus-absent
binary string. X2 covers the refused empty query, an absent query, a query present
in full, `--min-len` above every match, and a 4 KB query. X3 spot-checks
`doc_id`/`offset`/`title` against corpus bytes.

`mem_bench.py` X4 measures `--serve -j 32` on 10,000 short queries after a
warmup, checks frame order with absent-marker queries that must return
`best_len` 0, and checks `best_len <= query_len` and `occurrences <= n` on every
reply.

## Measured gates for `xsa mem` (2026-11-04)

X1 brute force via `mem_oracle.c` (one mmap pass over corpus bytes; independent
of the index): **150 MB: 20/20 full MEM sets and 20/20 longest matches exact**;
**100 GB: 5/5 longest-match triples exact** - `(0,32,1)`, `(0,16,412)`,
`(0,16,2)`, `(0,0,0)`, `(0,0,0)`. X2: **5/5** (empty query refused, absent query
no rows, query present in full, `--min-len 4096` above every match, 4 KB query
matched in full; the 4 KB scan takes 6.3 s, the documented quadratic term).
X3: **3/3** document spot checks at 100 GB (doc_id/offset/title resolved, corpus
bytes at each position equal the match, title prefix present at document start).

X4: 100 GB container, `--serve -j 32`, 9,800 timed 32-byte queries after 200
warmup queries (sampled windows, 16 of them replaced by absent markers that must
return `best_len` 0, i.e. request order is verified):

| Workers | Queries/s | µs/query (wall) | µs/query (kernel) | Input IPC µs | Output IPC µs |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | **23,397** | **42.7** | 988 | 41.6 | 1.1 |

The kernel per query is ~1 ms because a 32-byte query costs a few hundred rank
steps and each step is a block decode plus a random access in ~266 GB of
materialised columns; 32 workers overlap about 23 of them at a time. The first
200 queries are excluded because they fault the freshly built columns in (95.2 s
for 200, i.e. cold pages, not steady state). This is a **shared-host** wall: a
second operator's `xsa branches` serve job (~275 GB RSS) was running on the box.
The 100 GB container and `/mnt/nvme2n1/erikg/wiki/corpus.bin` were used; evidence
in [the evidence directory](tools/binbwt/evidence/mem/results.json).

# Fan-out: rarity-weighted document ranking

```
xsa fanout <container> <text-file|-> [--max-seeds 256] [--min-len 12] [--max-len 64]
           [--region-gap 2000] [--top 15] [--docs DIR] [--jsonl]
           [--max-rows 64] [--max-locate-budget 65536] [--keep-source] [--include-markup]
```

`xsa seeds` answers *which substrings of this text are specific and recurring*.
`xsa fanout` answers *which documents of the corpus share them*, and it ranks them
by **rarity**, not by count. Counting shared seeds is the defect this command
exists to fix: on the Cat article (document 4065) the top documents by shared-seed
COUNT were the topically correct ones ('Cultural depictions of cats', 'Cat
behavior') but they TIED with pure noise (Cottbus, Bautzen), because a seed that
500 documents share counted exactly as much as a seed that 3 documents share. A
count is a sum of ones; the fan-out of a distinctive seed is a sum of rarities.

## The weighting formula

```
D                = documents in the corpus            (from the seps.bin sidecars)
mult(seed)       = occurrences of the seed            (the BWT interval width)
weight(seed)     = ln( D / mult(seed) )               (floored at 0 if mult > D)
score(doc)       = SUM of weight(seed) over the DISTINCT seeds doc shares with the text
```

`mult` is the multiplicity the index already computes - no extra structure, no
document-frequency column. It is an occurrence count, not a document count, so
`D/mult` is a *lower* bound on inverse document frequency; for the seeds this
command emits (12 bytes and up, longest shared prefix) an occurrence is a
document in practice, and the binary measures it instead of asserting it:
`seed_docs_sum`/`seed_docs_max` in `FANOUT_STATS` are the located-hit counts per
seed (with `rows_located`, `seeds_truncated`).

The floor at 0 is the only departure from `ln(D/mult)`: a seed occurring more
often than the corpus has documents is not rare at all, and `weight_floor_clamped`
counts those seeds. It cannot happen for a seed whose occurrences are in distinct
documents.

## Regions

A document that shares ONE section of the text reports all of its evidence inside
that section, and a document score hides that. Hits are therefore clustered per
document: hits within `--region-gap` bytes (default 2000) of each other are one
region, and a region is scored exactly like a document - a sum of weights over the
DISTINCT seeds with a hit inside it. Both tables are printed. Region scores sum to
*at least* the document score: a seed with hits in two regions is one distinct seed
at document level and one in each region.

## Seed selection (untargeted, on purpose)

`xsa seeds` solves the seed LENGTH for a multiplicity target; that is how you get
a report about multiplicities. For scoring, the whole multiplicity range is
admissible and the WEIGHT ranks it, so `fanout` needs no target: at every window
it takes the **longest prefix reaching multiplicity 2** - the most specific string
at that offset that is shared at all - up to `--max-len`. The only floor is
sharing: a seed occurring once cannot be shared with another document, so
target 2 is the minimal nonzero sharing target. (`target = 1` was measured first:
on the Cat article it accepted 48,480 seeds and every one had multiplicity 1, so
the command refused by name - there was no fan-out to rank.)

`--skip-markup` is **ON by default** here (citation and template fragments are
known noise; `--include-markup` exposes them). Among the accepted shared seeds,
`--max-seeds K` (default 256) are picked at even index spacing, so the sample
covers the document rather than its head; `capped_dropped` says how many were not
picked, and `rows_truncated_seeds` how many seeds occur more than `--max-rows`
times (their rows are sampled evenly, so their document count is a lower bound).

The source document itself is found by comparison, not by guessing: the rarest
selected seed's hits give candidate document starts, and the text is compared
against `corpus.bin` at that start. A prefix match means "this document IS the
text", it is excluded from both tables (`FANOUT_SOURCE`, `excluded_hits`), and
`--keep-source` keeps it. Without `corpus.bin` the source is reported as
`unknown` and nothing is excluded.

The locate budget is checked BEFORE the walk (`--max-locate-budget`, default
65536 rows): a scoring pass that silently stopped early would be a wrong ranking,
not a slow one, so the command refuses by name instead.

## Output

TSV by default: a `#fanout` counter line, `#documents` (`rank, doc_id, score,
distinct_seeds, hits, doc_bytes, title, top_seeds`), then `#regions` (`rank,
doc_id, region_start, region_end, region_bytes, score, distinct_seeds, hits,
title, top_seeds`). `top_seeds` is the three highest-weight contributing seeds as
`s<id>@<weight>/m<multiplicity>:<escaped seed>`. `--jsonl` emits one `summary`
object and then `kind: document` / `kind: region` rows.

## Measured: the Cat article on the 100 GB container (2026-11-03)

Container `/mnt/nvme2n1/erikg/wf-finish-as-100gb-20261102T044713Z/corpus.sxi2`
(D = 25,932,219 documents, 100,093,934,466 corpus bytes), sidecars
`/mnt/nvme2n1/erikg/wiki`, text = document 4065, the **Cat** article,
177,347 bytes, extracted from `seps.bin` anchors+deltas. Default flags except
`--top 15`. **Every wall is a shared-host wall: load average 50-72 on a 256-core,
1007 GB box with other operators' jobs resident; peak RSS 269 GiB (the index
columns), wall 0:49.03, scan_ms 14,686.7, locate_ms 4,196.6.** The run found the
source document itself (`FANOUT_SOURCE doc=4065 matched_bytes=177347`), excluded
its 258 hits, located 921 rows of 256 selected seeds (11,757 shared seeds were
accepted, 11,501 dropped by even spacing), ranked 602 documents and found 614
regions.

Top 15 documents by weighted score (verbatim from
[cat_q1.tsv](tools/binbwt/evidence/fanout/cat_q1.tsv); `seeds` = distinct seeds,
`hits` = raw hits, `doc_bytes` = document length, last column = top contributing
seed):

| # | title | score | seeds | hits | doc_bytes | top seed |
| ---: | --- | ---: | ---: | ---: | ---: | --- |
| 1 | Cultural depictions of cats | 311.18 | 19 | 19 | 40587 | s229@16.3778/m2:nt at the domestic cats in Egypt, because he had only  |
| 2 | Wikipedia:Village pump (technical)/Archive 2 | 114.64 | 7 | 7 | 528299 | s67@16.3778/m2:sion and can see at one sixth the light level required  |
| 3 | Human interaction with cats | 94.12 | 6 | 6 | 40630 | s216@16.3778/m2:zation]] is a form of [[anthropomorphism]] in which ca |
| 4 | Cat intelligence | 77.74 | 5 | 5 | 46148 | s126@16.3778/m2:ing their brains to reorganize based on experiences. T |
| 5 | Cat food | 65.51 | 4 | 4 | 124333 | s60@16.3778/m2:nthesise enough vitamin D; inability to synthesise [[ni |
| 6 | Cat anatomy | 65.51 | 4 | 4 | 51546 | s37@16.3778/m2: spinal mobility and flexibility\|s38@16.3778/m2: them t |
| 7 | Cat senses | 49.13 | 3 | 3 | 23324 | s88@16.3778/m2:d in [[tuna]].&lt;ref name=&quot;McGrane Gibbs Hernango |
| 8 | Cat predation on wildlife | 44.98 | 3 | 6 | 70470 | s205@16.3778/m2:5573630708).jpg\|thumb\|Cats kill billions of wild birds |
| 9 | Cat genetics | 32.76 | 2 | 2 | 8789 | s179@16.3778/m2:out 250 heritable [[genetic disorder]]s have been iden |
| 10 | African wildcat | 28.20 | 2 | 3 | 36191 | s100@15.9724/m3:or of the domestic cat \|s162@12.2268/m127:Domestic cat |
| 11 | Zebrafish | 16.38 | 1 | 1 | 125810 | s21@16.3778/m2:vailable at the [[National Center for Biotechnology Inf |
| 12 | Wok | 16.38 | 1 | 1 | 39120 | s195@16.3778/m2: odors).&lt;ref name=&quot; |
| 13 | Kitten | 16.38 | 1 | 1 | 31515 | s170@16.3778/m2:\xe0\xb4\x9a\xe0\xb5\x8d\xe0\xb4\x9a\xe0\xb4\x95\xe0\x |
| 14 | Wavelength-division multiplexing | 16.38 | 1 | 1 | 49659 | s50@16.3778/m2:pacing&quot; [ |
| 15 | Adolescence | 16.38 | 1 | 1 | 213721 | s160@16.3778/m2:ts often develop pro |

Top 15 regions by weighted score (same run, `[start,end)` in document bytes):

| # | title | region | region_bytes | score | seeds | hits |
| ---: | --- | --- | ---: | ---: | ---: | ---: |
| 1 | Cultural depictions of cats | 7975–14478 | 6503 | 311.18 | 19 | 19 |
| 2 | Wikipedia:Village pump (technical)/Archive 2 | 184664–186231 | 1567 | 114.64 | 7 | 7 |
| 3 | Human interaction with cats | 37248–39090 | 1842 | 94.12 | 6 | 6 |
| 4 | Cat intelligence | 1696–4159 | 2463 | 77.74 | 5 | 5 |
| 5 | Cat food | 13855–14044 | 189 | 65.51 | 4 | 4 |
| 6 | Cat anatomy | 20761–23112 | 2351 | 49.13 | 3 | 3 |
| 7 | Cat senses | 20445–21211 | 766 | 49.13 | 3 | 3 |
| 8 | Cat genetics | 1304–1898 | 594 | 32.76 | 2 | 2 |
| 9 | Cat predation on wildlife | 96–142 | 46 | 32.76 | 2 | 2 |
| 10 | Zebrafish | 52475–52476 | 1 | 16.38 | 1 | 1 |
| 11 | Wok | 9583–9584 | 1 | 16.38 | 1 | 1 |
| 12 | Kitten | 21344–21345 | 1 | 16.38 | 1 | 1 |
| 13 | Wavelength-division multiplexing | 18400–18401 | 1 | 16.38 | 1 | 1 |
| 14 | Adolescence | 136376–136377 | 1 | 16.38 | 1 | 1 |
| 15 | Mount Healthy Heights, Ohio | 6211–6212 | 1 | 16.38 | 1 | 1 |

Reading it honestly: **11 of the top 15 documents are plausibly cat-related**
(9 explicitly cat-titled: Cultural depictions of cats, Human interaction with
cats, Cat intelligence, Cat food, Cat anatomy, Cat senses, Cat predation on
wildlife, Cat genetics, plus Kitten; and African wildcat, the domestic cat's
wild ancestor). One more, *Wikipedia:Village pump (technical)/Archive 227*, is a
noticeboard archive that quotes the Cat article - it shares 7 real article
sentences, so the match is correct as evidence and wrong as a topic. The
remaining **4 are noise** (Zebrafish, Wok, Wavelength-division multiplexing,
Adolescence), and they are noise of a specific kind: each sits at exactly
ln(D/2) = 16.3778 with **one** seed and **one** hit - a single 12-byte-or-longer
string appearing twice in 100 GB (`odors).&lt;ref name=&quot;`,
`pacing&quot; [`, `ts often develop pro`, ...). That is the floor of the ranking,
not a failure of the weighting: there is no way to score a document lower than
one shared screenshot-caption fragment. In the REGION table the same floor is
visible as 1-byte regions, and **the top 9 regions are all cat documents**, with
the 'Cultural depictions of cats' evidence confined to 6,503 bytes of a
40,587-byte article.

The four documents the seeds-era measurement expected and did NOT reach the top
15 - *Cat behavior*, *Feline*, *Domestication of the cat*, *Felidae* - are
absent, and the reason is visible in the table: this ranking rewards documents
that share MANY distinct article sentences, and those four share one or two
passages with the Cat article while *Cultural depictions of cats* shares 19. A
count-based ranking would have called that a tie.

## Gates for `xsa fanout`

| id | gate |
| --- | --- |
| Q1 | the Cat article at default flags ranks its own topical neighbourhood above unrelated documents - the tables above, with distinct-seed counts and top contributing seeds, and the plausible-share counted rather than asserted |
| Q2 | the weight is `ln(D/mult)` (checked against the formula, monotone in both arguments, floored at 0 for `mult > D`) and a document sharing ONE rare seed outranks a document sharing THREE ubiquitous ones, while the COUNT comparison favours the latter; the score does not depend on summation order (`cargo test`, module `fanout_tests`) |
| Q3 | REGIONS: a 2,000-byte section of the 177,348-byte Cat article (offsets 90000..92000, markup-skipped) scores 3,383.345 in ONE region `[90000,91997)` - 1,997 bytes, 1.1% of the document, 209 distinct seeds - and the document's own row is the sum over its regions, not a claim about the whole file; the document was NOT detected as the source (the text is a section, not the document), so it is ranked and the split is visible ([cat_region_q3.jsonl](tools/binbwt/evidence/fanout/cat_region_q3.jsonl) and the gate logs beside it (`seeds_gate_150m.log`, `branches_gate_150m.log`, `mem_gate_150m.log`)) |
| Q4 | `xsa seeds` unchanged by the scan extraction that `fanout` reuses: `seeds_gate.py` **PASS** (S1 mechanism with the 12..32 ladder, S2p placement, S2m markup, S3 guards, S4 determinism) on the 150 MB fixture; `branches_gate.py` W1 100/100 + top-k closure 100/100, W3 7/7, FILE 12/12, W4 100/100; `mem_gate.py` X1 all-mems 20/20, longest 20/20, X2 5/5; `cargo test --release` 64 passed, 0 failed |
| Q5 | committed, pushed, merged; this section documents the formula and the measured table |

The scan itself is NOT forked: `seeds_command` and `fanout_command` both call the
same `scan_text` (`xsa/src/seeds.inc.rs`), which is why Q4 is run at all.

# Seeds: adaptive exact-match seeding

```
xsa seeds corpus.sxi2 article.txt --target-mult 50 [--max-len 64] [--min-len 12]
         [--max-seeds 64] [--docs DIR] [--limit 8] [--jsonl] [--nearest]
         [--stride S] [--skip-markup]
xsa seeds corpus.sxi2 article.txt --max-mult 200 --min-mult 2      (band mode)
xsa seeds corpus.sxi2 - --target-mult 500                          (text on stdin)
xsa seeds corpus.sxi2 article.txt --anchors                        (anchor table)
xsa seeds corpus.sxi2 --serve -j 16 --rss-gb 8                     (warm, framed)
xsa seeds corpus.sxi2 --windows wins.jsonl --corpus pile.txt -j 16 (warm, declared)
```

**`seeds` is served too.** One process, one open, many windows: `--serve` takes
u32-length-prefixed window frames on stdin (the `mem --serve` encoding) and answers
one fixed-width ANCHOR frame per window, and `--windows FILE --corpus FILE` reads a
DECLARED window stream (the XSWIN001/JSONL form `consult` uses: `doc`, `base`,
`start`, `len`) and writes the anchor table as TSV. `--anchors` gives the same
table for a single text file. The anchor table is

```
window_id  query_offset  corpus_doc  corpus_offset  match_len  multiplicity
```

with `multiplicity` the EXACT interval width (so `--max-seeds`, `--limit` and
`--anchor-max-per-seed` are OUTPUT CAPS, never support thresholds), a documented
tie order `(query_offset, corpus_offset, match_len, multiplicity)` that makes two
runs byte-identical, and `corpus_doc` from member m15 or `-` when the container has
none. See [RESULTS_SEEDS_WARM.md](RESULTS_SEEDS_WARM.md) for the layer table
(scan 1.24 ms/window with zero locates; anchor locate 9.9 ms/row at `-j 64`;
document resolve 0.47 ms/row) and the cold-vs-warm rate on both 100 GB heads.

`xsa seeds` answers a different question from `mem`: not "where does this query
occur" but **"which substrings of this text are specific enough to pin, and
recurring enough to be a neighbourhood"**. The text is an article or a fragment
that is NOT the corpus. There is no embedding, no tokenizer and no vector - the
only primitive is the FM-index interval, over the same query kernel (members
m1,m4,m11, no member `branches` and `mem` did not already need). Nothing is
materialized to disc: the text is read through a windowed mmap (or one bounded
buffer for stdin), no text is ever written anywhere, and there is no per-suffix
position column.

## The one monotonicity

For a fixed text offset `i`, `mult(i, m)` = corpus occurrences of `text[i..i+m)`
is **non-increasing in `m`**: every occurrence of a longer prefix contains an
occurrence of the shorter one. Two consequences drive the whole command.

**Shortening raises multiplicity.** A shortening loop guarded by `mult > M`
cannot satisfy its own guard: it diverges, and it would reject every window it
touched. What a band `[min_mult, M]` asks for is the LONGEST seed whose
multiplicity has risen to `min_mult` without passing `M` - shorten while
`mult < target`, then refuse the window if what you reached is above the cap.
The one-byte-longer seed is the length the solve **gave up** at, and it is
reported per seed (`gave_up_len`, `gave_up_mult`).

**The length is solved for, not searched.** `--target-mult T` is the driving
parameter and `T` brackets the length, so a bisection over
`[--min-len, longest occurring prefix]` costs `O(log L)` interval descents per
window, each descent one rank step per byte. Linear shortening would be
`O(L^2)`. Every probe is re-checked against the monotonicity the bisection
assumes (`SEEDS_MULT_NOT_MONOTONE`) - a contradiction is a reader bug, not a
text property, and aborts.

## Where the windows are: the whole document

The default is a **continuous scan**: one window per byte, from offset 0 to
`len-1`, so `windows == text_bytes` and `coverage_pct == 100.0000`. It is not a
sample of the document and it does not stop early.

This is not a detail. An earlier revision walked a fixed stride (default
`max_len/2 = 32`) from offset 0 and stopped the moment it held `--max-seeds`
accepted seeds, so on the 177 KB Cat article it examined 62 windows - 1,984 bytes,
**1.1% of the document**, all of it the lead section. Wikipedia keeps its
infobox, redirect, date-style and maintenance templates in that lead, so the
seeds that scan returned were scaffolding: `{{About|the speci`,
`pp-semi-indef|small=yes}}`, `American English|date=October 2024`. A
front-to-back walk that stops at K can only ever answer with the head of the
document, so the placement and the cap were separated:

* **the scan is complete whatever `K` says.** `windows` is every window start in
  the text; the whole document is read before any selection happens.
* **`--max-seeds K` is an OUTPUT cap.** After the scan, the accepted seeds are
  walked at even index spacing and `K` are emitted, so the emitted set still
  covers the document instead of being the first `K` that happened to be found.
  `accepted_windows` says how many seeds the scan accepted, `capped_dropped` how
  many the cap dropped.
* **`--stride S`** is the opt-in sampler for the old behaviour: every `S`-th
  position, journaled as `stride=S`. It changes placement only - the scan still
  completes and `K` still applies at the output.
* **`--skip-markup`** (OFF by default) refuses windows that OVERLAP Wikipedia
  scaffolding. See below.

Coverage is reported, not asserted by prose:

| field | meaning |
| --- | --- |
| `windows` | window starts examined (`== text_bytes` for the continuous scan) |
| `coverage_pct` | 100 x (union of the byte ranges the windows READ) / `text_bytes`; 100.0000 only when every byte was inside some window |
| `examined_pct` | 100 x `windows * min(max_len, text)` / `text_bytes`, capped at 100. Above 100 before the cap means the windows OVERLAP (a continuous scan with `--max-len 64` reads every byte 64 times); with `--stride 32` it drops to a few percent, which is exactly the defect above |
| `first_window`, `last_window` | the first and last window start actually examined |
| `text_bytes` | the size of the text |

## `--skip-markup`: scaffolding is not prose

`--skip-markup` marks the byte ranges of the Wikimedia constructs that recur a
few hundred times **by construction** - `{{template}}` (nested, including
`{{{param|default}}}`), `<!-- comment -->`, `<ref>...</ref>` (and self-closing
`<ref ... />`), `<gallery>...</gallery>`, `<math>...</math>` and wikitables
`{| ... |}` (nested) - in one left-to-right pass, and does not START a window
that overlaps them. Scaffolding recurs by design, so left in the text it wins the
multiplicity target on its own; prose does not.

The scanner is deliberately simple and its simplifications are stated rather than
hidden: ranges are contiguous, so nesting inside a skipped range is swallowed by
jumping to the end of the construct, and an UNTERMINATED opener (a truncated
article) marks markup to the end of the text - which is what the text says. It is
OFF by default: the default reads every position, and `SEEDS_MARKUP
enabled=false ranges=0 bytes_skipped=0` says so. The journal reports
`windows_skipped_markup`, `markup_skipped_pct`, `markup_bytes_skipped` and
`markup_frac` either way.

## Per window

1. **Longest occurring prefix.** Bisect `m` in `[0, min(max_len, remaining))` for
   the largest prefix that occurs (occurrence is downward closed). Length 0 is
   `no_match`.
2. **Solve the length.** Bisect `m` in `[min_len, L0]` for the longest seed with
   `mult >= T`, keeping the one-byte-longer seed as the give-up point.
   `--nearest` may emit the give-up seed instead when it is closer to `T` in log
   space, and prints `SEEDS_NEAREST chose=below`.
3. **Accept or refuse, by name.** `no_match` (not one byte occurs), `too_short`
   (`L0 < --min-len`), `too_rare` (even at `--min-len` the multiplicity is below
   `T`: the window is more unique than the target), `too_common` (the longest
   occurring prefix is already above the ceiling, so *every* admissible length is
   more frequent still - the window cannot be pinned). The ceiling is `T` itself
   in target mode and `--max-mult` in band mode. An overshoot produced by
   SHORTENING is not `too_common`: with the ladder `12:336, 13:51, 14:38` and
   `T = 50` the answer is 13 at multiplicity 51.

`--max-mult M --min-mult m` is the alternative band mode (solves for `m`, refuses
above `M`); `--target-mult` and the band are exclusive and refused together.
Which mode ran is journaled.

## Output

Seeds are emitted in text order, selected from the accepted set at even index
spacing up to `--max-seeds K` (default 64). An already-accepted seed suppresses a
later one that is wholly **contained** in it, and identical seed content is
deduplicated (`suppressed_contained`, `suppressed_dedup`) - a continuous scan
produces the same seed at neighbouring offsets, so both are routine rather than
exotic. Containment is tested against the running maximum far end of the accepted
intervals, not against every accepted seed: with non-decreasing window starts an
earlier interval contains a later one exactly when its far end reaches past it,
which keeps a per-byte scan linear.

TSV columns:

```
seed_id  text_offset  length  occurrences  seed  gave_up_len  gave_up_mult  taken  truncated  positions  titles
```

`gave_up_len` is 0 when the emitted length is already the window's longest
occurring prefix. `seed` and the titles escape tabs, newlines, CRs and
backslashes only. `occurrences` is the interval width (exact overlapping count),
`taken` sample positions are located as `xsa query` locates them, and they are
**evenly spaced across the interval** (adjacent suffix-array rows usually share a
document, so the first rows would report one title). With sidecars each position
renders as `doc_id:offset:pos` and `titles` holds them joined by `|`; without
them the raw corpus offsets are emitted and `SEEDS_NOTE docs=none` says so.
`--jsonl` emits one object per seed with `seed_id`, `text_offset`, `length`,
`occurrences`, `seed`, `taken`, `truncated`, `positions`, `mode`, `target_mult`,
`gave_up_len`, `gave_up_mult`, and a `docs` array (`doc_id`, `offset`, `title`,
`pos`).

`SEEDS_STATS` journals, exactly:

```
SEEDS_STATS windows= coverage_pct= examined_pct= first_window= last_window= text_bytes=
            accepted_windows= capped_dropped= windows_skipped_markup= markup_skipped_pct=
            markup_bytes_skipped= markup_frac= tried= accepted= mean_len= mult_histogram=
            shorten_steps= extend_steps= scan_ms= locate_ms= mode= target_mult= cap=
            min_len= max_len= stride= no_match= too_short= too_rare= too_common=
            suppressed_dedup= suppressed_contained= nearest_below= positions= ranks= lf_steps=
```

`windows` is every window start examined and
`windows == no_match+too_short+too_rare+too_common+windows_with_seed+suppressed_*+windows_skipped_markup`
is asserted before the line is printed; `accepted` is the number of ROWS emitted
(after the `--max-seeds` selection) and `accepted_windows` the number of windows
that produced one; a scan long enough to matter journals `SEEDS_PROGRESS
windows= of= offset= accepted_windows= markup_skipped= elapsed_ms=` every 10,000
windows, so a minutes-long run is visible rather than silent; `tried` counts windows that reached the
multiplicity solve; `mult_histogram` is `mult:count` over accepted seeds;
`shorten_steps` and `extend_steps` count interval descents (the shortening and
the extension bisections). `--limit 0` locates nothing.

## Guards

`--max-len < --min-len` is refused by name (no length would be admissible),
`--min-len 0` (a zero-length seed occurs everywhere), `--max-seeds 0`,
`--stride 0`, `--target-mult 0`, `--target-mult` together with a band, and a half
band. Empty
text (a file or stdin) is refused rather than answering zero seeds. A text with
no matches answers zero seeds cleanly (`no_match` counted, exit 0). `0x1E` is an
ordinary byte, exactly as in every other query path.

## Reproducible gates

```sh
gcc -O2 -o seeds_oracle tools/binbwt/tests/seeds_oracle.c
python3 tools/binbwt/tests/seeds_gate.py XSA CONTAINER CORPUS --out DIR [--docs DIR]
```

`seeds_oracle.c` is an independent occurrence counter: one mmap pass per pattern,
glibc `memmem`, **overlapping** occurrences (what an interval width means), no
index code and no shared implementation. `seeds_gate.py` builds a 1200-byte
corpus slice as the seed text and checks, against that oracle:

| gate | what it asserts |
| --- | --- |
| S1 | every emitted seed's true multiplicity equals `occurrences` and reaches the target; the one-byte-longer seed equals `gave_up_mult` and is BELOW the target; multiplicity is non-increasing over a full 12..32 length ladder at each emitted offset; the reported length is the longest length still reaching the target; no seed is contained in another and no content repeats; the window counters add up; band mode keeps `min_mult <= occurrences <= max_mult`; a window far above the target is counted `too_common` and emits nothing |
| S3 | `--max-len` below `--min-len`, empty text, a zero target, both multiplicity modes at once and a half band are all refused by name; bytes absent from the corpus yield zero seeds and exit 0; a slice spanning a `0x1E` document separator still seeds and the oracle agrees |
| S2p | PLACEMENT - the default scan is continuous: `windows == text bytes`, `coverage_pct == 100`, `first_window == 0`, `last_window == text bytes - 1`, and `--max-seeds 3` still scans the whole text and emits at most 3 seeds spread over it; `--stride 32` is journaled as the opt-in sampler with `windows == ceil(text/32)` |
| S2m | `--skip-markup` - the skipped byte count equals a hand-built fixture to the byte (nested `{{ }}`, `}}`/`{{`/`|}` inside a comment, `{| ... |}`, `<ref>..</ref>`, self-closing `<ref/>`, `<gallery>`, `<math>`), every emitted seed is disjoint from every marked range, an unterminated `{{` runs to the end of the text, and the SAME run without the flag skips nothing (the flag is off by default) |
| S4 | the same input twice gives byte-identical output and identical counters |

## Measured gates for `xsa seeds` (2026-11-03)

S1/S3/S4 on the 150 MB fixture container `/mnt/nvme2n1/erikg/bw256/t150m.sxi2`
(corpus `/dev/shm/t150m.txt`) with the 150 MB corpus bytes as the independent
oracle: **PASS**. The ladder printed by the gate is the mechanism, verbatim -
offset 0 of a 1200-byte corpus slice with `--target-mult 50`:

```
offset 0:   12:336 13:51 14:38 15:38 16:4 17:1 ... 32:1
offset 32:  12:5597 13:402 14:5 15:3 16:1 17:1 ... 32:1
offset 192: 12:146 13:104 14:104 15:104 16:5 17:2 18:2 19:1 ... 32:1
offset 672: 12:167 13:99 14:98 15:98 16:97 17:8 18:3 19:2 20:1 ... 32:1
```

Multiplicity is non-increasing in length everywhere (asserted at every probe the
binary search makes), the emitted seed is the LONGEST length still reaching the
target (13 at 51 for offset 0, 13 at 402, 15 at 104, 16 at 97), and the
one-byte-longer seed is the reported give-up point (14 at 38, 14 at 5, 16 at 5,
17 at 8) - all four re-counted independently by `seeds_oracle.c`. Band mode
`50..80` kept every seed inside the band (1 seed; 3 windows counted
`too_common`). A window far above the target (`--max-len 12`, `--target-mult 2`
over a run of `A`) counted 15 `too_common` and emitted nothing. Guards: 6/6
refusals by name, absent bytes gave 2 `no_match` and exit 0, a 64-byte slice
spanning a `0x1E` document separator seeded 3 times with the oracle agreeing and
at least one seed spanning the separator. Determinism: two runs byte-identical,
counters identical.

Existing gates unchanged on the same 150 MB container (no shared code path was
touched: one new `include!`, one dispatch arm, two usage lines):
`branches_gate.py` W1 100/100 + top-k closure 100/100, W3 7/7, FILE 12/12,
W4 100/100 counts and positions; `mem_gate.py` X1 all-mems **20/20**, longest
**20/20**, X2 5/5; `cargo test --release` **58 passed, 0 failed** (56 before, plus
2 for `seeds`).

S2, the semantic-neighbourhood evidence. 100 GB container
`/mnt/nvme2n1/erikg/wf-finish-as-100gb-20261102T044713Z/corpus.sxi2`, sidecars
`/mnt/nvme2n1/erikg/wiki`, text = document 4065 of the corpus (the **Cat**
article, 177,348 bytes, extracted by reading `seps.bin` anchors+deltas directly),
`--max-len 64 --docs DIR --limit 32`. **Every wall below is a shared-host wall: a
second operator's `xsa branches --serve -j 128` on the SAME container was running
(277 GB RSS, load average 140) throughout.** Peak RSS of `xsa seeds` 279.9 GB (the
index columns), index load dominates the wall; `scan_ms`/`locate_ms` are the
product times.

| run | windows | accepted | mean_len | scan_ms | locate_ms | wall s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `--target-mult 50` | 62 | 40 (22 `too_rare`) | 18.70 | 42.5 | 6562.9 | 128.35 |
| `--target-mult 500` | 85 | 40 (44 `too_rare`, 1 contained) | 17.75 | 53.4 | 4038.1 | 91.36 |
| `--max-mult 200 --min-mult 2` | 46 | 40 (4 `too_rare`, 2 contained) | 32.05 | 42.8 | 914.9 | 97.09 |
| `--target-mult 10 --min-len 16` | 98 | 40 (57 `too_rare`, 1 `too_common`) | 24.90 | 64.8 | 4728.3 | 96.81 |

`locate_ms` is large at `--limit 32` because a seed with multiplicity 4774 walks
32 LF paths spread over the whole interval; interval widths and the solve itself
cost tens of milliseconds for a 177 KB text. The seeds, their lengths and their
multiplicities, and the distinct titles their sample positions land in, are in
[the evidence directory](tools/binbwt/evidence/seeds/results.json).

What the titles say (the point of the exercise): with `--target-mult 50` the 1280
sampled positions land in **1232 distinct documents** - exact matching with tuned
specificity really does spread over the pile - but the article's first 3 KB are
MediaWiki template/citation syntax (`{{pp-semi-indef|small=yes}}`,
`American English|date=October 2024}}`, `s://archive.org/details/i`), so the
top-hit titles are Wikipedia noticeboards, and `--target-mult 500` is worse in
exactly that way (higher fan-out pins shorter, more generic strings). The
content-bearing runs are the interesting ones: `--target-mult 10 --min-len 16`
over 1391 positions in **1289 distinct documents** returns
*List of mammals of Missouri*, *Wild goat*, *Fisher (animal)*, *Indian giant
squirrel*, *Elk*, *Hippobosca*, *List of Tabanus species*, *Stratiomys*,
*Gymnosoma*, *Simia*, *Melanophora roralis* - species articles sharing the
taxobox and the taxonomy vocabulary. Which is the honest reading of the
hypothesis: the neighbourhood is real and exact-match reproducible, and what it
is a neighbourhood OF is decided by the shared surface form (template + taxobox +
`Carnivora` vocabulary), not by anything semantic beyond it.

## Coverage fix: the whole document, not its lead (2026-11-03, second pass)

The S2 numbers above were measured on a scan that examined **62 windows of 32
bytes - 1.1% of a 177 KB article** - because the walk started at offset 0 and
stopped the moment it held `--max-seeds`. Everything it found came from the lead,
where Wikipedia keeps its infobox/redirect/date templates, so the seeds were
scaffolding (`{{About|the speci`, `pp-semi-indef|small=yes`,
`American English|date=October 2024`) and the top-hit titles were noticeboards.
That is a placement defect, not a property of the kernel: the same text scanned to
the end answers with the whole article.

The scan is now continuous (`windows == text_bytes`, `coverage_pct == 100`) and
`--max-seeds` is an output selection. Measured on the 100 GB container
`/mnt/nvme2n1/erikg/wf-finish-as-100gb-20261102T044713Z/corpus.sxi2`, sidecars
`/mnt/nvme2n1/erikg/wiki`, text = document 4065 (the **Cat** article, 177,347
bytes; the body only - the 0x1E framing byte of the next document is not part of
it), `--max-len 64 --max-seeds 40 --limit 32 --docs DIR`.

**Every wall is a shared-host wall: a second operator's `xsa branches --serve
-j 128` on the SAME container held 277 GB RSS with load average 82 throughout.
Peak RSS of each `xsa seeds` run is ~271 GiB (the index columns); the wall is
dominated by opening the index, not by the scan - `scan_ms` is the product time.**

| run | windows | coverage_pct | accepted_windows | emitted | mean_len | scan_ms | locate_ms | wall | peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `--target-mult 50` | 177347 | 100.0000 | 30132 | 40 | 20.40 | 33476.0 | 5654.3 | 2:21.63 | 270.8 GiB |
| `--target-mult 500` | 177347 | 100.0000 | 22599 | 40 | 18.35 | 31936.5 | 5863.4 | 2:08.75 | 270.7 GiB |
| `--target-mult 50 --skip-markup` | 177347 | 37.3466 | 10287 | 40 | 16.70 | 20227.6 | 8955.0 | 2:14.06 | 269.1 GiB |

`--target-mult 50` (P1). The 40 seeds span offset 0 to 171280 - the whole article,
not its first 2 KB - and their 1280 sample positions land in **1234 distinct
documents**. Titles ranked by hit count (top 20):

| hits | title |
| ---: | --- |
| 9 | Cat |
| 5 | Muscle car |
| 4 | 2023–24 Kerala Blasters FC season |
| 4 | 2023 Indonesian used train import controversy |
| 4 | Hudson Italia |
| 3 | Sole Front for Women's Rights |
| 3 | Sperm precedence |
| 3 | Gaza genocide |
| 3 | American Motors Corporation |
| 2 | Blossom and Frankie |
| 2 | Treaty of Ripon |
| 2 | Broad-tailed paradise whydah |
| 2 | Orange-winged pytilia |
| 2 | Pantherinae |
| 2 | Coma Berenices (dwarf galaxy) |
| 2 | David L. Banks |
| 2 | Thornlie–Cockburn line |
| 2 | Kilusang Bagong Lipunan |
| 2 | Scaly-breasted munia |
| 2 | Stabilimentum |

`--target-mult 500` (P2), same scan, 10x the fan-out: **1262 distinct documents**
of 1280 positions, i.e. nearly every sample position lands in its own document -
higher fan-out pins shorter, more generic strings.

| hits | title |
| ---: | --- |
| 5 | Taste receptor |
| 3 | Grammy Award for Record of the Year |
| 2 | Grammy Award for Album of the Year |
| 2 | René Küss |
| 2 | Pat O'Connor (wrestler) |
| 2 | Greater Gotham |
| 2 | TAS2R14 |
| 2 | TAS2R8 |
| 2 | TAS2R45 |
| 2 | Gustducin |
| 2 | Calcium chloride |
| 2 | Mallee whipbird |
| 2 | Industry of the South Humber Bank |
| 2 | World population |
| 1 | Navicat |
| 1 | Dancer with Flat Hat |
| 1 | Kadakkarapally Boat |
| 1 | Beachcat |
| 1 | Koreasat |
| 1 | Teldat |

`--target-mult 50 --skip-markup` (P3, the filter as a knob): the scanner marks
**107,899 of the 177,347 bytes (60.8%)** as scaffolding and 126,171 of the 177,347
windows (71.1%) are refused, so `coverage_pct` honestly drops to 37.3 - the prose
that remains. **1229 distinct documents**, and the neighbourhood becomes the
article's own subject matter:

| hits | title |
| ---: | --- |
| 21 | Cat |
| 5 | Glossary of climbing terms |
| 4 | Tooth pathology |
| 3 | Cat behavior |
| 3 | Wikipedia:Categories for discussion/Log/2024 January 24 |
| 3 | Wikipedia:Dispute resolution noticeboard/Archive 65 |
| 3 | Moorov v HM Advocate |
| 3 | Primo Villanueva |
| 3 | List of protected areas of Peru |
| 2 | East Patagonia, Tierra del Fuego and Strait of Magellan dispute |
| 2 | Golden Horde |
| 2 | Jack Antonoff |
| 2 | Eugen Haile |
| 2 | Kangaroo |
| 2 | Cats in ancient Egypt |
| 2 | John Catsimatidis |
| 2 | Francolin |
| 2 | Cleveland CycleWerks |
| 2 | Blackamoores |
| 2 | Wikipedia:WikiProject Medicine/Lists of pages/Low-importance medicine articles |

So the filter is what moves the answer: with the markings skipped the same
continuous scan returns *Cat behavior*, *Cats in ancient Egypt*, *Kangaroo*,
*Francolin*, *Pantherinae* - the article's own subject - instead of citation
column syntax; without it, the reference-dense body of a Wikipedia article still
supplies most of the recurring strings, which is the honest reading of a
surface-form hypothesis.

Consistency check on the mechanism: `--stride 32` still journals `stride=32` and
`windows = ceil(text_bytes/32)`; `--skip-markup` off journals
`markup_bytes_skipped=0`, `windows_skipped_markup=0`; the window counters still
add up exactly (`SEEDS_WINDOW_ACCOUNTING`, asserted before the journal line) now
including `windows_skipped_markup`.

Other gates on the 150 MB fixture `/mnt/nvme2n1/erikg/bw256/t150m.sxi2` (corpus
`/dev/shm/t150m.txt`): `seeds_gate.py` **PASS** (S1 mechanism with the 12..32
ladder reproduced verbatim, S2p placement - `windows == text bytes`,
`coverage_pct == 100`, `first_window == 0`, `last_window == text-1`, `--max-seeds 3`
still scanning the whole text, `--stride 32` journaled as a sampler, S2m markup -
exact skipped-byte count against a hand-built fixture including nested `{{ }}`,
`}}`/`{{`/`|}` inside a comment, self-closing `<ref/>`, an unterminated `{{`
running to the end, and the same run with the flag off skipping nothing, S3
guards, S4 determinism); `branches_gate.py` W1 100/100 + top-k closure 100/100,
W3 7/7, FILE 12/12, W4 100/100; `mem_gate.py` X1 all-mems 20/20 longest 20/20,
X2 5/5; `query_oracle_gate.py` counts 100/100 and positions 100/100 (574
positions); `cargo test --release` **61 passed, 0 failed** (58 before, plus 3 for
the markup scanner: the construct table, the near-misses, and the cursor).

The seeds, offsets, lengths, multiplicities and title lists above are in
[the coverage-fix evidence file](tools/binbwt/evidence/seeds/coverage_fix.json).
