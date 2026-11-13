# `xsa consult` — bounded-memory streaming extraction (cache-build mode)

**Status:** implemented, gated, and it **supersedes the python orchestration
path** (`flagship_union_service.py` → `xsa branches --serve` → python row
assembly). One native process now reads windows, queries the container, and
appends durable per-position rows under a hard RSS budget. The emender's 400 GB
RSS accumulation is structurally impossible here: there is no object in this
process whose size is a function of the number of positions consumed.

Requirement document: `NOTE_FROM_EMENDER_EXTRACTION_PERFORMANCE.md` (the
"Interface contract" section), deliverable A.

```
xsa consult --container <sxi2> --windows <jsonl|bin> --output <file>
            [--corpus FILE] [--rss-gb 8] [-j N] [--resume] [--left 1024]
            [--chunk-rows N] [--cache-gb G] [--bin|--jsonl] [--strict]
            [--no-verify-prefix] [--docs DIR] [--exclude-source]
            [--doc-restrict] [--doc-restrict-cap N]
```

The removed `--sep-cut` / `--no-sep-cut` are hard errors that name the reason:
the `0x1E` spine cut inferred document boundaries from content bytes and was
wrong on every document that contains an internal `0x1E` (78,715 of them over
the full pile; `NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md`).

---

## 1. What one position is

For a position at corpus byte offset `off` inside a document whose first byte is
`base` (explicit in the window, or resolved from the document structure by
`doc` — see §2):

1. `ctx = corpus[max(base, off - min(left, 1024)) .. off]` — the spine, cut at
   the **document base**, `max(0, off - base)` bytes. **Never a byte scan**: the
   `0x1E` separator cut is REMOVED. A separator inside a document is content
   like any other byte, and document boundaries are METADATA
   (`NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md`): at vocabulary 256 no in-band
   delimiter can be safe by construction, and the pile's own framing collides
   with `0x1E` inside 78,715 documents.
2. **exactly one query**: the next-byte branch histogram of `context || c` over
   the container, for every present byte `c`, plus the structural-END
   occurrence. No backoff, no second context length, no reuse cache. An empty
   spine (the position is the document's first byte) is an **empty histogram**
   (not the global corpus histogram).
3. With `--doc-restrict` (needs a document structure): the histogram is
   **restricted to the document containing the position** — a match or branch
   count never spans a document boundary, exactly rather than probabilistically.
   Exactness costs enumeration: every occurrence of the context is located and
   kept only if the match lies fully inside the document AND its continuation
   byte does too; intervals wider than `--doc-restrict-cap` (default 1024) are
   **refused loudly**, never silently returned unrestricted.
4. With `--exclude-source`: the position's **own occurrence is removed before
   anything is reported** (its branch byte is the corpus byte AT the position).
   Reported `width` then means "occurrences other than the one at this position",
   and the row's flag bit 2 states that the removal was applied
   (their receipted C14 leakage: `RESULT_C14_IN_INDEX_LEAKAGE.md`).

This is `branch_result`'s answer (`xsa branches --serve -k 256`) with the
redundant full backward search removed: `consult_answer` calls the *same*
`extension_counts` (one-sided right-extension by pure rank arithmetic) and
recovers `width`/`eof` from the counts, using `branch_result`'s own asserted
identity

```
width = sum(counts) + eof            eof = 1  <=>  the spine is a suffix of the text
```

`--strict` recomputes every row through `branch_result` and refuses to write a
byte of disagreement. Measured: `--strict` and the fast path produce the *same*
row file sha256, and that sha256 also matches the pre-optimisation build that
answered every row with `branch_result`.

## 2. Windows (input)

The cursor is a **whole-window** cursor: a commit never splits a window.

### JSONL (one object per line)

```json
{"doc": 0, "base": 3000000512, "start": 0, "len": 2048, "left": 1024}
```

| field | meaning |
|---|---|
| `doc` | document id: carried into every row, and — when `base` is absent — the id the document structure resolves the base from (`select(doc)`) |
| `base` | absolute corpus byte offset of the document's **first** byte. OPTIONAL: absent means "resolve it from the structure by `doc`". REFUSED when no structure is available (no member m15 in the container and no `--docs DIR`) — a base is never inferred from content bytes |
| `start` | 0-based position index inside the document of the window's first position (default 0) |
| `len` | number of consecutive positions, 1..1048576 (default 1). A window with **neither `base` nor `len`** is the **whole document** `doc` (every position, base resolved from the structure) |
| `left` | left-context budget in bytes, `<= 1024` (default `--left`) |

Position `i` of the window is corpus offset `base + start + i`, and its spine is
bounded below by `base`. Extra fields are ignored; a missing `doc` is a hard
error; a missing `base` is a hard error **only** when no document structure is
available. A last line without a trailing newline is refused (a torn window set
is never guessed at). Blank lines are skipped.

**The document structure** is, in precedence order: the `--docs DIR` sidecar
(`seps.bin`, XSEP v1 — the `xsa corpus --pile-map --out DIR` output; a map that
COVERS the text may be a superset, so the full 1.3 TB pilemap resolves any
head-prefix container), and the container's own member **m15** (SXI2 v10,
`xsa build --docs-map DIR` or `xsa docstarts embed`). When both are present they
are cross-checked (`select` at 64 random doc ids, `rank` at 64 random
positions) and a disagreement is a hard error: two authorities that disagree
about a boundary cannot both be obeyed. When a structure is present and a
window states a `base` that DISAGREES with it (the position's containing
document starts elsewhere), the run is refused — boundary parity in production,
per position.

### Binary batch (`--bin`)

32-byte records, native little-endian; the **first record is a header**
(`XSWIN001` + 24 zero bytes), then one record per window:

```
u64 doc | u64 base | u64 start | u32 len | u32 left        (left == 0xFFFFFFFF -> --left;
                                                             base == 0xFFFF_FFFF_FFFF_FFFF -> resolve from doc)
```

A file whose size is not a nonzero multiple of 32 is refused. A binary record
always carries `len`, so the whole-document form is JSONL-only.

## 3. Rows (output)

`--output` is a new file. Header, then one record per position, appended in
window order (positions ascending, windows as given). **Version 2** (the v2
row is the v1 row plus a 24-byte tail: `docid`, `base`, `depth_in_doc` — one
row answers both "what continues this" and "inside which document"; the
note's D2). A reader that expects version 1 must refuse version 2, and vice
versa — the tail changes every record boundary after the first:

```
header:  "XSACONS1" (8 B) | u32 version = 3 | u32 reserved = 0
row:     u32 nsym          number of (byte, count) pairs, 0..=256
         u16 ctxlen        spine length actually queried, 0..=1024
         u16 flags         bit0 EOF present, bit1 spine truncated by the 1024 cap,
                           bit2 source-excluded (--exclude-source removed the
                               position's own occurrence),
                           bit3 doc-restricted (the histogram is restricted to
                               the document containing the position),
                           bit4 boundary_exact=0: this row's histogram is the
                               boundary-UNAWARE conflated answer (see §5.4); a
                               consumer MUST be able to filter per row, and this
                               bit is how
         u64 doc           the window's document id, as given
         u64 off           absolute corpus offset of the position
         u64 width         NON-CROSSING occurrences of the spine (see §5.4):
                           every occurrence counted once, on a content branch
                           or the END branch; with bit2 set: occurrences OTHER
                           than the one at this position
         u64 eof           occurrences of the spine that END a document (a
                           document starts at off+ctxlen), plus the one
                           structural END (the spine is a suffix of the text).
                           v2 readers that assumed eof in {0,1} MUST be updated:
                           on Pile text this is commonly in the thousands.
         nsym x { u8 byte | u64 count }
         u64 docid         the CONTAINER's document id from the structure
                           (member m15 / --docs), or 0xFFFF_FFFF_FFFF_FFFF when
                           there is no structure or the position's document is
                           not a container document
         u64 base          the window's effective document base (explicit or
                           resolved from the structure)
         u64 depth         off - base (depth in document)
```

Record size is `40 + 9*nsym + 24` bytes. Pairs are ordered **count descending,
byte ascending** (the same order `xsa branches` emits). Everything is
little-endian. Nothing is compressed and nothing is buffered on the side.

**Version 4** (`--rows-v4`, OPT-IN — the default output stays v3 and
byte-identical, because downstream experiments are hash-bound to the row
bytes): every record is preceded by a 1-byte `kind`, and a **FORCED** row — an
answer from the width-1 or one-step-verification layers, a single pair
`(cont, width-eof)` — drops its 9-byte pair and takes flag **bit 6**:

```
header:  "XSACONS1" (8 B) | u32 version = 4 | u32 reserved = 0
row:     u8  kind         0 = FULL (the v3 record verbatim)
                           1 = FORCED:
         u16 ctxlen | u16 flags (bit0 eof, bit1 truncated, bit2 excluded,
                                  bit3 restricted, bit4 boundary_exact=0,
                                  bit6 forced) |
         u64 doc | u64 off | u64 width | u64 eof | u8 cont |
         u64 docid | u64 base | u64 depth
```

A FORCED record is 62 bytes where the v3 form of the same row is 73; the pair
decodes to `(cont, width - eof)` by construction (`width > eof` always).
`--resume` refuses to continue a v4 file with a v3 run and vice versa (the
kind byte changes every record boundary).

*Reading the map to the emender's `FLAGSHIPSPARSE2` row* (`flagship_cache.py`,
`ROW = <HBBH`, `PAIR = <BQ`): `context_bytes = ctxlen`, `eof = eof`,
`censored = (ctxlen > 0)`, `pairs = [(byte,count)]` exactly as above,
`context_count = width`, `support_size = width - eof`, `distinct_branches` and
`entropy` computed from the pairs as their reader already does. A next byte of
`0x1E` (decimal 30) is emitted like any other byte, exactly as their chain
records it and their `features()` excludes it from the probabilities. Their
`write_window` geometry (`start % 2048 == 0`, `1 <= len <= 2048`) is satisfied by
the T2048 window sets we benchmark with. The v2 tail maps as: their document id
`docid` (the CONTAINER's id, not the window's opaque `doc`), the document base
`base`, and `depth_in_doc`; `excluded = (flags & 4) != 0`, `restricted =
(flags & 8) != 0`, and with `excluded` set their `context_count`/`support_size`
must be read from the EXCLUSION-adjusted `width` (it already is: the width field
IS the adjusted number).

## 4. Journal (`<output>.journal`)

Append-only, one record per line, `key=value`, fsync'd per record:

```
CONSULT_OPEN      container/index_n/index_r/corpus/text_bytes/output/jobs/rss_cap_bytes/
                  cap_gb/chunk_rows/left_default/windows/windows_format/batch_positions/
                  arena_bytes/cache_bytes/index_rss_cap_bytes/docs_source/docs/exclude_source/
                  doc_restrict/doc_restrict_cap
CONSULT_RSS       rss_gb=… rss_bytes=… cap_gb=… positions=… reorder_batches=… pending_rows=…
                  backpressure_events=…            (about once a second)
CONSULT_BACKPRESSURE state=pause|resume positions=… cause=worker_queue_full|rss_budget
                                                     [rss_gb=… cap_gb=… waited_ms=…]
CONSULT_COMMIT    seq=… windows_done=… windows_bytes_done=… positions_done=…
                  output_bytes=… rows_sha256=<sha256 of the whole committed prefix>
CONSULT_RESUME    seq/journal_lines/journal_torn_tail_lines/truncated_bytes/prefix_verified/
                  windows_done/windows_bytes_done/positions_done/output_bytes
CONSULT_DONE      positions/windows/seconds/positions_per_s/output_bytes/rows_sha256/
                  last_commit_seq/commits/peak_rss_gb/peak_rss_bytes/cap_gb/jobs/
                  cache_hits/cache_misses/block_decodes/drop_epochs/decode_ns/drop_ns/
                  prefetch_calls/corpus_remaps/backpressure_events/windows_bytes_done/
                  excluded_rows/restricted_rows
```

## 5. Guarantees

### 5.1 The memory law (bounded, by construction)

Four independent things keep RSS under `--rss-gb`:

1. **No accumulation.** A row is encoded straight into the batch buffer that is
   already in flight and written by the writer thread; the buffer is recycled.
   There is no per-position cache, reuse map, reorder log or result list. The
   only per-position state is inside the batch that carries it.
2. **A bounded in-flight window.** At most `jobs * 8` batches of
   `BATCH_POSITIONS (64)` positions exist, each holding its spine bytes
   (≤1 KiB). Reaching the limit *blocks the producer* — that is the
   backpressure, journaled as `CONSULT_BACKPRESSURE state=pause|resume`.
3. **The RSS governor.** The container is mapped read-only; the reader's own
   arena is **fixed at open** (`--cache-gb`, default `cap/8`) and
   `decoded_bytes` stays 0. Only the resident set of the mapping's directories
   can grow, and it is bled off in 1 GiB windows (`madvise(MADV_DONTNEED)`)
   whenever the process crosses `--rss-gb`.
4. **The corpus is a windowed mmap.** At most 64 MiB of the text is mapped at a
   time, and a retired window is `MADV_DONTNEED`'d before it is dropped. No text
   is written anywhere, ever.

Measured with `--rss-gb 8` on the 100 GB pile container: **peak RSS 7.99 GB**
(`VmHWM`, the kernel's number), i.e. the budget is the ceiling, not a hope. The
same run at `--rss-gb 16` peaks at 11.4 GB (nothing to fill the rest: the
directories are 5.9 GB) and produces the *same bytes*.

### 5.2 Durability and resume

Rows are appended, `fdatasync`'d, and **then** the commit record is appended and
fsync'd. A crash can therefore leave an uncommitted row tail, never a committed
byte that is not on disk. A chunk is committed only at a window end, so the
cursor is `(windows_done, windows_bytes_done)`.

`--resume`:

1. takes the **last complete** `CONSULT_COMMIT` record (a torn journal tail — a
   line without a newline — is discarded, never trusted);
2. refuses if the output is *shorter* than the commit's `output_bytes` (committed
   bytes are gone) ;
3. truncates the output back to `output_bytes` (the torn row tail is dropped);
4. re-hashes the committed prefix and **refuses to continue** if it does not
   match `rows_sha256` (`--no-verify-prefix` skips that check knowingly);
5. continues at the recorded window cursor.

### 5.3 Determinism

Every row is a pure function of the window set and the container: one query,
fixed context length, counts, a total order on the pairs. Thread count, RSS cap,
arena size, batch granularity and crash/interruption therefore cannot change a
byte. Verified in §6 (A2): two runs, kill+resume, and three different `--rss-gb`
caps all give the same sha256.

### 5.4 The no-cross law: a match or branch count never spans a document boundary

THE LAW (the owner's semantic requirement, default on): for an occurrence of
the spine at position `pos` (length `L`), the continuation `q = pos + L` is

- the **END branch** ("this context ends a document") iff `q` is a document
  start — `select(rank(q)) == q` over the structure (member m15 or `--docs`) —
  or `q == text_len` (the one structural END);
- the **content byte** `corpus[q]` otherwise;

and the occurrence is a **crossing match**, counted nowhere, iff a document
start lies inside `(pos, q-1]` (`rank(pos) != rank(q-1)`). `width` counts the
non-crossing occurrences; `eof` counts the doc-ending ones plus the structural
END; the content pairs never include the first byte of the NEXT document. In
`pile.txt` the byte before a document start is the framing `0x1E` and belongs
to the document that owns it: an occurrence ending at that framing byte is the
END branch, and an internal `0x1E` (446 in the first 10 GB) is CONTENT — the
distinction is the whole point, and it is metadata that makes it, never bytes.

**How the default answers, in layers** (all exact where they answer — the
χ-proportional cascade; zero-locate layers answer by pure rank arithmetic):

1. **The boundary-aware trie** (member m20 with `boundary_aware=1`): a context
   within the trie's depth whose node is materialised is answered from the
   member itself — O(depth), any width, no locate. The trie enforces the law BY
   CONSTRUCTION, so this layer has no conflated cases at all.
2. **WIDTH-1**: one backward search of the full context (the existing rank
   machinery, zero locate). A raw width of 1 means the ONLY occurrence in
   the whole text is the query's own (the spine is real text cut at the
   document base), and the own occurrence is always an in-document content
   continuation (`off < doc_next`, `off < text_len`) — a crossing or END
   occurrence would be a SECOND one. The row is `{(own, 1)}`, `eof=0`, no
   matter what the container's framing is. **88.1% of the rehearsal census.**
3. **The zero-locate raw layers** — THE SEPARATOR-FRAMING LAW: on a corpus
   whose every in-text document start `d` is immediately preceded by one
   `0x1E` byte (the Pile's own framing — `xsa corpus --pile-map` builds
   exactly this), a context that contains NO `0x1E` byte cannot touch a
   boundary at all: a CROSSING occurrence would need a document start `d`
   inside `(pos, at-1]`, and `corpus[d-1] = 0x1E` would then have to equal
   `C[d-1-pos]`; an END-branch occurrence (its continuation at a document
   start) would need `C[ctxlen-1] = 0x1E`; and the framing byte at a
   document's content end is a CONTENT byte of the document that owns it.
   With `C` 0x1E-free every occurrence is therefore in-document content or
   the one structural text end — exactly the raw backward-search
   classification — so **the no-cross histogram IS the raw histogram, at
   any width, with zero locate**. The law is verified EXHAUSTIVELY at open
   (`CONSULT_SEPFRAMING`: one 1-byte pread per in-text document start,
   16,119,766 of them in ~2 s warm on the rehearsal corpus); a container
   that fails it (adversarial fixtures whose boundaries sit mid-content,
   any map not built by the pile framing) keeps the exact enumeration for
   EVERY row — exactness is never traded for speed. A spine that carries an
   internal `0x1E` (content bytes of its own document; 446 documents in the
   first 10 GB have one) gets no proof, so it falls through conservatively.
   Under the law:
   - **SINGLE-CHILD SHORT-CIRCUIT**: the trie node of the context's LAST
     ≤depth bytes with exactly one child forces the byte — a fork can never
     widen going deeper, because every occurrence of the full context is a
     non-crossing occurrence of that suffix at the SAME continuation
     offset. The own occurrence is always one of the node's counted content
     children, so the single child must be the position's own byte; a
     disagreement dies as a corrupt member. ~8 node walks instead of a
     2×ctxlen rank chain.
   - **ONE-STEP VERIFICATION**: candidate `b` = the query's own continuation
     from the window; append and search once. `width(C·b) == width(C) -
     eof` means every occurrence continues with `b` — the row is
     `{(b, W-eof)}`, zero locate. A narrowing means fork, conservatively.
     NOTE the trap this avoids: the equality can hold while a CROSSING
     occurrence hides inside (a crossing row that continues with the same
     byte), so the equality alone is never trusted — it is trusted only
     together with the framing proof, and the adversarial gate constructs
     exactly that trap both ways (unframed corpus, and a framed corpus
     whose 0x1E-carrying spine hides a junction match).
   - **RAW FORK**: the raw extension-count histogram (the full one-sided
     right-extension by rank arithmetic — the same machinery
     `xsa branches` uses), exact at any width within `--boundary-cap`.
4. **Exact enumeration**: every occurrence in the context's interval is
   located (one sampled-SA LF walk each) and classified by the law (two m15
   rank/selects per occurrence, one corpus pread per content branch).
   Bounded by `--boundary-cap` (default 8192).
5. **The marked fallback**: a context wider than the cap AND deeper than
   the trie keeps the conflated answer with **flag bit 4 set**
   (`boundary_exact=0`) — the bytes stay what they were before the
   rewrite (the raw layers stay capped ON PURPOSE: a too-wide row must keep
   today's marked bytes, downstream experiments are hash-bound to them).
   Every run journals `BOUNDARY_INEXACT rows=<n> total=<n> ratio=<f>`.

Measured on the rehearsal census (682,496 banked rows): 88.1% width-1,
10.6% single-branch — the forced mass — answered by layers 2–3 with zero
locate; the whole fork mass answers by the raw histogram (the law covers
any width, so forks need no locates either); only 0x1E-carrying spines
(~0.1% at the Pile's internal-separator rate) and the marked tail
enumerate. Consult's cost is now proportional to χ (the fork skeleton),
not to n: the 13.7 positions/s locate-bound baseline became 3,285/s
measured (240×) coexisting with the live pile build, at `--rss-gb 8`.

Every row is charged to a layer with its nanosecond cost into a bounded
log2 histogram; the journal closes with `CONSULT_LAYERS` (rows / ns / p50
per layer) and `CHI_ZEROLOCATE` (the zero-locate mass share).

`--cross-doc` opts the WHOLE run back into the legacy conflated answer (every
row marked, journaled); `--boundary-strict` converts the marked fallback into a
refusal for exactness-or-nothing consumers; `--boundary-cap N` is the cost
knob. With NO structure at all (no m15, no `--docs`) every row is the marked
conflated answer plus a loud note at open — the run is never silently wrong,
and it is never fatal by default.

`--doc-restrict` keeps its stricter contract (occurrences within the query
position's own document) and now applies the same END rule: an occurrence
whose continuation lands on the document end counts on the END branch (it
used to be dropped), and a match spanning past the end is a crossing match.

## 6. Gates (verbatim)

Container `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2` (63.16 GB, n =
100000000001, R = 31381597694, candidates = 243). Corpus
`/mnt/nvme2n1/erikg/pile.txt` (its first 100e9 bytes ARE this container's text;
the container's own `n-1` bounds every position and the EOF test).

### A1 — correctness: exact parity with a brute-force scan

`gates/consult_oracle.c` scans **the whole 100 GB text** for every context and
counts what follows each occurrence — no index, no FM search, no code shared with
the thing under test. 35 contexts (lengths 1,2,3,4,5,6,8,12,16,24,32,40,48,64,96,
128,192,256,300,384,512,768,900,1000,1024, spread over the first 40 GB, plus
positions right after a `0x1E` and two at the end of the text) are answered
TWICE by `xsa consult` — once with the raw spine (cut at the document base
only: the separator scan is gone, and the rows after a `0x1E` prove it: their
spine INCLUDES the framing byte), and once with `--exclude-source`, whose
width and pairs must be the raw answer minus the position's own occurrence
(branch byte = the corpus byte AT the position, the driver subtracts it and
compares) — so 70 rows and 10.6 billion individual counts are compared:

```
A1_PARITY contexts=35 rows_checked=70 exact=35 mismatched=0 counts_compared=10611404039 exclude_rows=35 result=PASS
```

(excerpt of the per-row lines; `width` is the occurrence count of the spine, so
this compares multi-billion-count histograms, not just branch sets)

```
A1_ROW idx=0 len=1 width=8492344394 eof=0 pairs=171 parity=exact
A1_ROW idx=17 len=384 width=1 eof=0 pairs=1 parity=exact
A1_ROW idx=21 len=1024 width=1 eof=0 pairs=1 parity=exact
A1_ROW idx=30 len=1024 width=1 eof=0 pairs=1 excl_width=0 parity=exact
A1_ROW idx=34 len=1024 width=1 eof=0 pairs=1 excl_width=0 parity=exact
```

`--strict` additionally recomputes every row through `branch_result` (the
`branches --serve` path) and aborts on any disagreement; on 1024 positions the
strict and fast row files have the same sha256, and that sha256 is also the one
the earlier `branch_result`-only build produced (`1300afb8…`).

### A2 — determinism: three shas, all the same

`gates/consult_a234.sh` (re-run on the structural-docs lane, 2026-11-11; the
row shas differ from the pre-lane receipt **by design** — the spine is cut at
the document base now, not at the last `0x1E`, and the row layout is v2 — while
the gate's own comparisons are unchanged and all pass):

```
A3_RUN1 positions=1001472 positions_per_s=4556.521 peak_rss_gb=7.998 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_RUN2 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_TWO_RUNS identical=1 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_CAP rss2=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f rss8=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f rss16=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f
A2_WINDOW_FORMAT jsonl=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f bin=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f
A2_FULL_1M rss8=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0 rss16=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_DETERMINISM two_runs=1 kill_resume=1 rss_cap_2v8v16=1 window_format=1 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
```

The two window forms agree too — the same 51,200-position set given as JSONL and
as the binary batch (`--bin`) produces the same file
(`A2_WINDOW_FORMAT jsonl=9695eb01… bin=9695eb01…`).

(a) two runs of the same 1M-position window set: identical. (b) kill+resume:
identical (A4). (c) `--rss-gb 2` vs `8` vs `16`: identical, on the 51,200-position
set (`rss2 = rss8 = rss16`) and on the full 1M set (`rss8 = rss16`). Output is
also invariant to `-j` (4/8/16/32/64/96) and to `--cache-gb`.

### A3 — bounded: peak RSS 8.00 GB at `--rss-gb 8`

Full runs of the same 1M-position window set, all the same bytes, on a box whose
foreign load moved between them (four FastGA/FastGCD jobs at ~15 cores each plus
GPU training jobs):

```
A3_BOUNDED positions=1001472 positions_per_s=4556.521 peak_rss_gb=7.998 cap_gb=8 jobs=96   (structural-docs lane, load ~60)
A3_BOUNDED positions=1001472 positions_per_s=5096.514 peak_rss_gb=7.987 cap_gb=8 jobs=96   (structural-docs lane, run 2)
CONSULT_STATS positions=1001472 positions_per_s=5151.090 peak_rss_gb=15.997 cap_gb=16 jobs=96 (structural-docs lane, --rss-gb 16)
CONSULT_STATS positions=1001472 positions_per_s=5920.775 peak_rss_gb=14.122 cap_gb=16 jobs=96 (pre-lane receipt, load ~50)
```

**Honest verdict on the 5,000/s bar: it is met on average and not by a margin.**
At the 8 GB cap the lane's rates span 4,557-5,097 positions/s under heavier
foreign load than the pre-lane runs (4,957-5,641, mean 5,236) - the bar is
inside the spread, and the box was never quiet. At the 16 GB cap that the
contract allows (8-16 GB) it is 5,151-5,921 positions/s, because the RSS
governor stops bleeding page tables (`drop_epochs` 641 -> 0). Peak RSS is
`VmHWM` from `/proc/self/status`, and it is journaled every second as
`CONSULT_RSS rss_gb=… cap_gb=…`: with `--rss-gb 8` the process rides **at** its
budget (8.00 GB), which is the budget working, not a hope.

The window set is their cache geometry: 489 T2048 windows at 2048-aligned
offsets, spine = last ≤1024 bytes cut at the **document base** (the lane's rule;
the pre-lane receipt cut at the last `0x1E`), mean `ctxlen` ~709 of 1024.

### A4 — crash and resume, with a real torn tail

```
A4_KILL exit=124 (124 = the harness's SIGTERM, the crash this gate needs)
NO_SURVIVORS work=/mnt/nvme2n1/erikg/sd-scratch/a234
A4_KILL file_bytes=35854379 last_commit_output_bytes=30626268 commits=2
A4_RESUME positions=1001472 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0 truncated_bytes=5228111
A4_RESUME_IDENTICAL to_uninterrupted_run=1 positions=1001472 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
```

The killed run had appended 35,854,379 bytes but committed 30,626,268, so
`--resume` discarded a real **5,228,111-byte torn tail**, verified the committed
prefix's sha256 (`prefix_verified=1`), and finished with the uninterrupted run's
exact bytes, byte for byte, at the same row count.

Every launch in the harness is `setsid timeout --kill-after=10 N`, and every step
asserts that no `xsa consult` process of ours survived it (`NO_SURVIVORS`).

### Reproducing the gates

```sh
cargo build --release --manifest-path xsa/Cargo.toml
python3 gates/consult_a1.py /mnt/nvme2n1/erikg/pile.txt \
        /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 xsa/target/release/xsa /tmp/a1
bash gates/consult_a234.sh xsa/target/release/xsa \
        /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 /mnt/nvme2n1/erikg/pile.txt /tmp/a234 96
# the structural-docs gates (needs a container with member m15; the banked
# container + `xsa docstarts embed --out` makes one):
bash gates/structdocs_g1.sh xsa/target/release/xsa CONTAINER-M15 \
        /mnt/nvme2n1/erikg/pilemap_full /tmp/g1 10000 100000000000
python3 gates/structdocs_fixtures.py xsa/target/release/xsa XSA-ADADF48 /tmp/g23
```

`consult_a1.py` compiles `consult_oracle.c` itself; `structdocs_g1.sh` compiles
`docstarts_oracle.c`. The structural-docs lane's runs are receipted in
`RESULTS_STRUCTURAL_DOCS.md` (commit and shas there); the pre-lane A1/A234
receipt above was made at commit `80d2951` with binary sha256
`a7c289c61955d57061bfb8ea31e2513d836b95cc6e037813a70affa8c8b97c65`.

### The χ-proportional gates (G1–G4, RESULTS_CHI_CONSULT.md)

```sh
bash gates/chi_g3_law.sh xsa/target/release/xsa XSA-BASE /tmp/chi-g3
#   G3 THE LAW: adversarial fixtures (same-byte crossing traps, END-adjacent,
#   internal-0x1E, widths 2-4) against the brute-force law oracle AND byte
#   parity with the pre-change binary. XSA-BASE is the pre-change binary
#   (built from the base commit of this branch).
bash gates/chi_g1_parity.sh xsa/target/release/xsa XSA-BASE \
        /mnt/nvme2n1/erikg/chi-consult/g10g/pf-10gb.sxi2 \
        /mnt/nvme2n1/erikg/pf-10gb.txt /tmp/map10g /tmp/chi-g1 16
#   G1 PARITY: the same window set through the pre-change binary and the chi
#   binary on the 10 GB full-stack container — every row byte-identical,
#   forced rows equal the old path's single-pair histograms, the opt-in v4
#   file decoding back to exactly the same rows.
bash gates/chi_g2_census.sh xsa/target/release/xsa \
        /mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2 \
        /mnt/nvme2n1/erikg/pile-head100.txt /tmp/chi-g2 16
#   G2 CENSUS: the full rehearsal window set; the banked 682,496-row prefix
#   byte-for-byte (fork and marked rows must match); the measured layer mix
#   next to the census (88.1/10.6/0.7/0.5).
bash gates/chi_g4_rate.sh xsa/target/release/xsa \
        /mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2 \
        /mnt/nvme2n1/erikg/pile-head100.txt /tmp/chi-g4 16
#   G4 RATE: cold + warm at --rss-gb 8, per-layer p50s, aggregate positions/s
#   against the 13.7/s locate-bound baseline (>=100x asserted).
```

## 7. Throughput, and what it is measured on

One process, this box, with **foreign load throughout** (four FastGA/FastGCD jobs
at ~15 cores each plus GPU training jobs; `load average` 24 -> 99 while the gates
ran, which is why repeats of the same command vary by ~15%). Source: the
`CONSULT_STATS` / `CONSULT_DONE` journal records. Walls include container open
(`Index::open`) and the corpus window map.

| window set | spine | positions | `-j` | positions/s | peak RSS | sha256 (short) |
|---|---|---|---|---|---|---|
| T2048 x 489 (their geometry) | ≤1024, cut at `0x1E`, mean 709 | 1,001,472 | 96 | **5,641.0** | 7.99 GB | `679984ed…` (pre-lane receipt; the lane's rule cuts at the document base and gives `6058073f…`) |
| T2048 x 489 | same | 1,001,472 | 96 | **5,109.6** | 7.99 GB | `679984ed…` (pre-lane) |
| T2048 x 489 | same | 1,001,472 | 96 | 4,957.0 | 8.00 GB | `679984ed…` (pre-lane) |
| T2048 x 489, `--rss-gb 16` | same | 1,001,472 | 96 | **5,920.8** | 14.12 GB | `679984ed…` (pre-lane; the lane's run gives `6058073f…` at 5,151.0) |
| T2048 x 489, resume from 401,408 | same | 599,064 | 96 | 8,539.3 | 7.99 GB | `679984ed…` (pre-lane) |
| T2048 x 7,849 | same | 200,704 | 16 | 2,284.0 | 7.99 GB | `16b7a268…` (pre-lane) |
| T1024 x 16, raw 1024-byte spines | 1024 for every position | 16,384 | 96 | **4,956.9** | 5.63 GB | `8bcd491d…` (pre-lane, when `--no-sep-cut` still existed; the lane's spine is always the raw document-base cut) |
| T1024 x 16, raw 1024-byte spines | 1024 for every position | 16,384 | 64 | 4,533.5 | 5.63 GB | `8bcd491d…` (pre-lane) |

For scale: the emender's receipt (`receipts/c14_cache_scale/benchmark_summary.json`,
1,034,056 positions, `jobs=128`) is **6,088.7 positions/s** with a peak RSS of
**295 GB**, at a mean `context_bytes` of **14.9** and **6.9M native queries**
(≈6.7 per position). This path answers the same 1M-position geometry at
~5.1-5.6k positions/s with **one** query per position and a mean spine of **709**
bytes (≈48x the rank work per position: `xsa branches` reports
`rank_steps_per_context=4225` for a 1024-byte spine, so ~62 by proportion at
15 bytes) in **7.99 GB**. Per position that is ~240x less wall and
~37x less memory.

Where the throughput came from (measured, `-j 32`, 1024-byte spines, pile):

| change | positions/s |
|---|---|
| as first written | 1,047 |
| cache-line-padded `Shard` (false sharing on the per-lookup counters) | 2,433 |
| + `consult_answer` drops the redundant full backward search | 3,073 |
| + a 1 GiB arena instead of 64 MiB (fewer random block decodes) | 4,177 (`-j 64`) |

Remaining headroom, and why it is not taken here: the kernel's parallel
efficiency is ~25-30% (`-j 96` gives ~7x of one core's rate) and it is memory
latency / page-walk bound - `rank` walks the container's 5.9 GB of directories at
random, which no amount of streaming can fix. Closing that gap is deliverable B's
maintained-ladder work, not this stream's.

The RSS governor's own cost is visible and measurable: at `--rss-gb 8` the mapping
is pinned at the budget and `drop_epochs` runs in the hundreds (641 on a 1M run),
each bleeding a 1 GiB window of page-table entries; at `--rss-gb 16`
`drop_epochs` is 0 and the same workload is 13% faster. The smaller arena
experiment (`--cache-gb 0.5`) is *slower* (4,651/s, 121M block decodes) - the
arena earns its share of the budget.

## 8. Differences from the emender python chain, and what to adapt

1. **One query per position, not 6.7.** Their `select` binary-searches the
   longest occurring spine suffix (a validity rule for a sampled corpus) and pays
   ~6.7 queries per position. `xsa consult` queries the spine once. For a
   position whose spine occurs — every position of a corpus the container covers
   — the two agree exactly. If their window sets ever point at text the container
   does not cover, their selector would shorten the spine and we would answer
   with the (absent) full one.
2. **The spine is cut at the DOCUMENT BASE, not at `0x1E`** (D1): their
   `c14_branch_serve.select` rule `rsplit(b'\x1e', 1)` inferred boundaries from
   content bytes and is wrong on every document with an internal `0x1E`. Give
   `base` per window (or let `doc` resolve it from member m15 / `--docs DIR`);
   the backoff is still not implemented. With `--doc-restrict` the counts are
   exact per document; with `--exclude-source` the width excludes the position's
   own occurrence (their receipted C14 leakage).
3. **The row layout is ours** (§3, v2) — a documented fixed-layout record with
   the `docid`/`base`/`depth_in_doc` tail; their reader adapts once (mapping in
   §3) or reads our layout directly. A v1 reader must refuse a v2 file (the
   tail moves every record boundary).
4. **Their `censored` bit** means "the backoff search was capped at the available
   context". We have no backoff, so for any nonzero `ctxlen` their reader should
   set `censored = 1`;
5. **No python, no IPC.** Rows are assembled in the same process that queries, so
   the 121k/s assembly and the 5.9k/s orchestration walls of their chain both
   disappear from the path.

## 9. Residual risks and limits

- **Document-restricted counting is ENUMERATIVE and capped** (§1): `--doc-restrict`
  locates every occurrence of the context (one LF walk each) and keeps only the
  ones whose match AND continuation byte lie inside the position's document, so
  it is exact but costs O(width) locates per row. Intervals wider than
  `--doc-restrict-cap` (default 1024) are REFUSED, never silently returned
  unrestricted — raise the cap (and pay), drop the flag, or query longer
  contexts. Long-spine cache builds (mean ctxlen 709) have width ~1 and pay
  ~nothing; short-context workloads (C14-style, mean 14.9 B) can see six-figure
  widths and must choose.
- **`--doc-restrict` is opt-in**: base resolution, per-position boundary parity
  and the docid/base/depth row fields are automatic when a structure is present,
  but counting semantics only change when the flag is given — the refusal above
  is the honest alternative to a silent slowdown.
- **`--strict` skips restricted rows**: the strict cross-check is defined
  against `branch_result` (the unrestricted rank kernel); restricted rows are
  proved exact by the fixture gate's brute-force oracle instead.
- **Throughput on the hardest workload is only just over the bar** (§7): a full
  1024-byte spine for every position is ~50x the rank work of their measured
  workload, and the pre-existing query kernel's parallel efficiency (~25% at
  64-96 threads on a box with foreign load) is what limits it, not this stream.
- **Fixture containers cannot be built on this box for very small inputs** (the
  tape route's fold floor needs a ~36 MB corpus at `--threads 4`, ~141 MB at
  `--threads 32`; the bin route dies in `xsa_finish` with "the sentinel row is
  missing or duplicated"), so the adversarial fixtures carry a 48 MiB pad and the
  100 GB brute-force oracle runs against the real container (gate A1).
- **The absent-context path is unreachable from a window** (§1): a spine read
  from the corpus occurs at its own position, so `width >= 1` whenever the spine
  has no structural-END byte. The guard stays as a defence.
- **The window file is not hashed.** Cursor identity is `(windows_done,
  windows_bytes_done)`; the contract says their window sets are frozen and
  hash-bound, and we do not re-derive their hash.
- **`eof` is not reachable from a window** (same reason) — the field exists for
  row-layout parity and is exercised by `--strict`.
- The RSS governor's cost is real: at `--rss-gb 8` the mapping is pinned at the
  budget and `drop_epochs` runs in the hundreds, each bleeding 1 GiB of PTEs.
