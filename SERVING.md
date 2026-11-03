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

This v6 lite container has no move/psi member. Its sparse m11 anchors contain
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
SXI2 v6 lite kernel, using no container member that `branches` did not already
need. It **supersedes the legacy `xsa mems` for v6 lite containers**:
`mems` routes through the phi-era reader and refuses these containers by name
("SXI: invalid header").

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

# Seeds: adaptive exact-match seeding

```
xsa seeds corpus.sxi2 article.txt --target-mult 50 [--max-len 64] [--min-len 12]
         [--max-seeds 64] [--stride 32] [--docs DIR] [--limit 8] [--jsonl] [--nearest]
xsa seeds corpus.sxi2 article.txt --max-mult 200 --min-mult 2      (band mode)
xsa seeds corpus.sxi2 - --target-mult 500                          (text on stdin)
```

`xsa seeds` answers a different question from `mem`: not "where does this query
occur" but **"which substrings of this text are specific enough to pin, and
recurring enough to be a neighbourhood"**. The text is an article or a fragment
that is NOT the corpus. There is no embedding, no tokenizer and no vector - the
only primitive is the FM-index interval, over the same v6 lite kernel (members
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

## Per window

`--stride S` (default `max(1, max_len/2)`) is the window step.

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

Seeds are emitted in text order and stop at `--max-seeds K` (default 64). An
already-emitted seed suppresses a later one that is wholly **contained** in it,
and identical seed content is deduplicated (`suppressed_contained`,
`suppressed_dedup`).

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
SEEDS_STATS windows= tried= accepted= mean_len= mult_histogram= shorten_steps= extend_steps=
            scan_ms= locate_ms= mode= target_mult= cap= min_len= max_len= stride=
            no_match= too_short= too_rare= too_common= suppressed_dedup=
            suppressed_contained= nearest_below= positions= ranks= lf_steps=
```

`windows` is every window start examined and
`windows == no_match+too_short+too_rare+too_common+windows_with_seed+suppressed_*`
is asserted before the line is printed; `tried` counts windows that reached the
multiplicity solve; `mult_histogram` is `mult:count` over accepted seeds;
`shorten_steps` and `extend_steps` count interval descents (the shortening and
the extension bisections). `--limit 0` locates nothing.

## Guards

`--max-len < --min-len` is refused by name (no length would be admissible),
`--min-len 0` (a zero-length seed occurs everywhere), `--max-seeds 0`,
`--target-mult 0`, `--target-mult` together with a band, and a half band. Empty
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
| S4 | the same input twice gives byte-identical output and identical counters |
