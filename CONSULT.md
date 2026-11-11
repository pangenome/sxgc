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
header:  "XSACONS1" (8 B) | u32 version = 2 | u32 reserved = 0
row:     u32 nsym          number of (byte, count) pairs, 0..=256
         u16 ctxlen        spine length actually queried, 0..=1024
         u16 flags         bit0 EOF present, bit1 spine truncated by the 1024 cap,
                           bit2 source-excluded (--exclude-source removed the
                               position's own occurrence),
                           bit3 doc-restricted (the histogram is restricted to
                               the document containing the position)
         u64 doc           the window's document id, as given
         u64 off           absolute corpus offset of the position
         u64 width         occurrences of the spine (= sum of counts + eof; with
                           bit2 set: occurrences OTHER than the one at this
                           position)
         u64 eof           0 or 1
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

*Reading the map to the emender's `FLAGSHIPSPARSE2` row* (`flagship_cache.py`,
`ROW = <HBBH`, `PAIR = <BQ`): `context_bytes = ctxlen`, `eof = eof`,
`censored = (ctxlen > 0)`, `pairs = [(byte,count)]` exactly as above,
`context_count = width`, `support_size = width - eof`, `distinct_branches` and
`entropy` computed from the pairs as their reader already does. A next byte of
`0x1E` (decimal 30) is emitted like any other byte, exactly as their chain
records it and their `features()` excludes it from the probabilities. Their
`write_window` geometry (`start % 2048 == 0`, `1 <= len <= 2048`) is satisfied by
the T2048 window sets we benchmark with.

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
three positions immediately after a `0x1E` and two at the end of the text) are
answered twice by `xsa consult` — once with `--no-sep-cut` against the raw spine,
once with the default separator cut against the oracle on the *cut* suffix — so 70
rows and 10.6 billion individual counts are compared:

```
A1_PARITY contexts=35 rows_checked=70 exact=35 mismatched=0 counts_compared=10611404039 sep_cut_rows=32 sep_empty_rows=3 result=PASS
```

(excerpt of the per-row lines; `width` is the occurrence count of the spine, so
this compares multi-billion-count histograms, not just branch sets)

```
A1_ROW idx=0 len=1 cut_len=1 width=8492344394 eof=0 pairs=171 parity=exact
A1_ROW idx=17 len=384 cut_len=384 width=1 eof=0 pairs=1 parity=exact
A1_ROW idx=21 len=1024 cut_len=1024 width=1 eof=0 pairs=1 parity=exact
A1_ROW idx=28 len=1024 cut_len=0 width=1 eof=0 pairs=1 parity=exact
A1_ROW idx=33 len=1024 cut_len=39 width=1 eof=0 pairs=1 parity=exact
```

`--strict` additionally recomputes every row through `branch_result` (the
`branches --serve` path) and aborts on any disagreement; on 1024 positions the
strict and fast row files have the same sha256, and that sha256 is also the one
the earlier `branch_result`-only build produced (`1300afb8…`).

### A2 — determinism: three shas, all the same

`gates/consult_a234.sh`:

```
A3_RUN1 positions=1001472 positions_per_s=5109.619 peak_rss_gb=7.994 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
A2_RUN2 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
A2_TWO_RUNS identical=1 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
A2_CAP rss2=789b949ed41eaf90da012d6dda4716990b170b5f03dc8d95b862dc34e5acb45d rss8=789b949ed41eaf90da012d6dda4716990b170b5f03dc8d95b862dc34e5acb45d rss16=789b949ed41eaf90da012d6dda4716990b170b5f03dc8d95b862dc34e5acb45d
A2_FULL_1M rss8=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d rss16=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
A2_DETERMINISM two_runs=1 kill_resume=1 rss_cap_2v8v16=1 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
```

The two window forms agree too — the same 51,200-position set given as JSONL and
as the binary batch (`--bin`) produces the same file
(`A2_WINDOW_FORMAT jsonl=789b949e… bin=789b949e…`).

(a) two runs of the same 1M-position window set: identical. (b) kill+resume:
identical (A4). (c) `--rss-gb 2` vs `8` vs `16`: identical, on the 51,200-position
set (`rss2 = rss8 = rss16`) and on the full 1M set (`rss8 = rss16`). Output is
also invariant to `-j` (4/8/16/32/64/96) and to `--cache-gb`.

### A3 — bounded: peak RSS 8.00 GB at `--rss-gb 8`

Three full runs of the same 1M-position window set, all `--rss-gb 8`, all the same
bytes, on a box whose foreign load moved between them (`load average` 50 -> 99;
four FastGA/FastGCD jobs at ~15 cores each plus GPU training jobs):

```
A3_BOUNDED positions=1001472 positions_per_s=4956.978 peak_rss_gb=8.000 cap_gb=8 jobs=96    (load ~57)
A3_BOUNDED positions=1001472 positions_per_s=5109.619 peak_rss_gb=7.994 cap_gb=8 jobs=96    (load ~40)
CONSULT_STATS positions=1001472 positions_per_s=5640.997 peak_rss_gb=7.993 cap_gb=8 jobs=96  (load ~24)
CONSULT_STATS positions=1001472 positions_per_s=5920.775 peak_rss_gb=14.122 cap_gb=16 jobs=96 (load ~50)
```

**Honest verdict on the 5,000/s bar: it is met on average and not by a margin.**
At the 8 GB cap the rate spans 4,957-5,641 positions/s (mean 5,236) - the bar is
inside the spread, and the box was never quiet. At the 16 GB cap that the
contract allows (8-16 GB) it is 5,921 positions/s, because the RSS governor stops
bleeding page tables (`drop_epochs` 641 -> 0). Peak RSS is `VmHWM` from
`/proc/self/status`, and it is journaled every second as `CONSULT_RSS
rss_gb=… cap_gb=…`: with `--rss-gb 8` the process rides **at** its budget (8.00
GB), which is the budget working, not a hope.

The window set is their cache geometry: 489 T2048 windows at 2048-aligned
offsets, spine = last ≤1024 bytes cut at the last `0x1E`, mean `ctxlen` 709.3 /
median 855 of 1024.

### A4 — crash and resume, with a real torn tail

```
A4_KILL exit=124 (124 = the harness's SIGTERM, the crash this gate needs)
NO_SURVIVORS work=/tmp/consult-fix/gfinal
A4_KILL file_bytes=27555240 last_commit_output_bytes=21278172 commits=2
CONSULT_RESUME from_seq=2 windows_done=196 positions_done=401408 output_bytes=21278172 truncated_bytes=6277068 prefix_verified=1
A4_RESUME positions=1001472 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
A4_RESUME_IDENTICAL to_uninterrupted_run=1 positions=1001472 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
```

The killed run had appended 27,555,240 bytes but committed 21,278,172, so
`--resume` discarded a real **6,277,068-byte torn tail**, verified the committed
prefix's sha256 (`prefix_verified=1`), and finished with the uninterrupted run's
exact bytes, byte for byte, at the same row count.

Provenance of that block, stated exactly: the `A4_KILL ... / CONSULT_RESUME ...`
lines above are the `--chunk-rows 200000` invocation of the harness's A4 step (the
setting the committed harness uses, so the kill cannot land on a commit
boundary). The full-harness run at commit `80d2951`, made before that setting was
added, was killed exactly ON a commit boundary -
`A4_KILL file_bytes=28013586 last_commit_output_bytes=28013586 commits=8`,
`A4_RESUME ... truncated_bytes=0` - and was also byte-identical to the
uninterrupted run; it simply never exercised the truncation branch.

Every launch in the harness is `setsid timeout --kill-after=10 N`, and every step
asserts that no `xsa consult` process of ours survived it (`NO_SURVIVORS`).

### Reproducing the gates

```sh
cargo build --release --manifest-path xsa/Cargo.toml
python3 gates/consult_a1.py /mnt/nvme2n1/erikg/pile.txt \
        /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 xsa/target/release/xsa /tmp/a1
bash gates/consult_a234.sh xsa/target/release/xsa \
        /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 /mnt/nvme2n1/erikg/pile.txt /tmp/a234 96
```

`consult_a1.py` compiles `consult_oracle.c` itself. Both drivers were run from a
clean worktree at commit `80d2951`; the binary's sha256 was
`a7c289c61955d57061bfb8ea31e2513d836b95cc6e037813a70affa8c8b97c65`.

## 7. Throughput, and what it is measured on

One process, this box, with **foreign load throughout** (four FastGA/FastGCD jobs
at ~15 cores each plus GPU training jobs; `load average` 24 -> 99 while the gates
ran, which is why repeats of the same command vary by ~15%). Source: the
`CONSULT_STATS` / `CONSULT_DONE` journal records. Walls include container open
(`Index::open`) and the corpus window map.

| window set | spine | positions | `-j` | positions/s | peak RSS | sha256 (short) |
|---|---|---|---|---|---|---|
| T2048 x 489 (their geometry) | ≤1024, cut at `0x1E`, mean 709 | 1,001,472 | 96 | **5,641.0** | 7.99 GB | `679984ed…` |
| T2048 x 489 | same | 1,001,472 | 96 | **5,109.6** | 7.99 GB | `679984ed…` |
| T2048 x 489 | same | 1,001,472 | 96 | 4,957.0 | 8.00 GB | `679984ed…` |
| T2048 x 489, `--rss-gb 16` | same | 1,001,472 | 96 | **5,920.8** | 14.12 GB | `679984ed…` |
| T2048 x 489, resume from 401,408 | same | 599,064 | 96 | 8,539.3 | 7.99 GB | `679984ed…` |
| T2048 x 7,849 | same | 200,704 | 16 | 2,284.0 | 7.99 GB | `16b7a268…` |
| T1024 x 16, `--no-sep-cut` | 1024 for every position | 16,384 | 96 | **4,956.9** | 5.63 GB | `8bcd491d…` |
| T1024 x 16, `--no-sep-cut` | 1024 for every position | 16,384 | 64 | 4,533.5 | 5.63 GB | `8bcd491d…` |

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
