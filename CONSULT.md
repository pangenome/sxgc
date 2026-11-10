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
            [--chunk-rows N] [--cache-gb G] [--bin|--jsonl] [--no-sep-cut]
            [--strict] [--no-verify-prefix]
```

---

## 1. What one position is

For a position at corpus byte offset `off` inside a document whose first byte is
`base`:

1. `ctx = corpus[off - min(left, 1024, off - base) .. off]` — the spine, the last
   ≤1024 bytes, never crossing the document start.
2. the spine is cut at the **last `0x1E`** inside it (their verified rule; a
   context never spans a document boundary). A position immediately after a
   separator therefore has an empty spine. `--no-sep-cut` keeps the raw spine.
3. **exactly one query**: the next-byte branch histogram of `context || c` over
   the container, for every present byte `c`, plus the structural-END
   occurrence. No backoff, no second context length, no reuse cache. An empty
   spine is an **empty histogram** (not the global corpus histogram).

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
| `doc` | opaque document id, carried into every row |
| `base` | absolute corpus byte offset of the document's **first** byte |
| `start` | 0-based position index inside the document of the window's first position (default 0) |
| `len` | number of consecutive positions, 1..1048576 (default 1) |
| `left` | left-context budget in bytes, `<= 1024` (default `--left`) |

Position `i` of the window is corpus offset `base + start + i`, and its spine is
bounded below by `base`. Extra fields are ignored; a missing `doc`/`base` is a
hard error. A last line without a trailing newline is refused (a torn window set
is never guessed at). Blank lines are skipped.

### Binary batch (`--bin`)

32-byte records, native little-endian; the **first record is a header**
(`XSWIN001` + 24 zero bytes), then one record per window:

```
u64 doc | u64 base | u64 start | u32 len | u32 left        (left == 0xFFFFFFFF -> --left)
```

A file whose size is not a nonzero multiple of 32 is refused.

## 3. Rows (output)

`--output` is a new file. Header, then one record per position, appended in
window order (positions ascending, windows as given):

```
header:  "XSACONS1" (8 B) | u32 version = 1 | u32 reserved = 0
row:     u32 nsym          number of (byte, count) pairs, 0..=256
         u16 ctxlen        spine length actually queried, 0..=1024
         u16 flags         bit0 EOF present, bit1 spine truncated by the 1024 cap
         u64 doc           from the window
         u64 off           absolute corpus offset of the position
         u64 width         occurrences of the spine (= sum of counts + eof)
         u64 eof           0 or 1
         nsym x { u8 byte | u64 count }
```

Record size is `40 + 9*nsym` bytes. Pairs are ordered **count descending, byte
ascending** (the same order `xsa branches` emits). Everything is little-endian.
Nothing is compressed and nothing is buffered on the side.

*Reading the map to the emender's `FLAGSHIPSPARSE2` row* (`flagship_cache.py`,
`ROW = <HBBH`, `PAIR = <BQ`): `context_bytes = ctxlen`, `eof = eof`,
`censored = (ctxlen > 0)`, `pairs = [(byte,count)]` exactly as above,
`context_count = width`, `support_size = width - eof`, `distinct_branches` and
`entropy` computed from the pairs as their reader already does. Their
`write_window` geometry (`start % 2048 == 0`, `1 <= len <= 2048`) is satisfied by
the T2048 window sets we benchmark with.

## 4. Journal (`<output>.journal`)

Append-only, one record per line, `key=value`, fsync'd per record:

```
CONSULT_OPEN      container/index_n/index_r/corpus/text_bytes/output/jobs/rss_cap_bytes/
                  cap_gb/chunk_rows/left_default/windows/windows_format/batch_positions/
                  arena_bytes/cache_bytes/index_rss_cap_bytes/sep_cut
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
                  prefetch_calls/corpus_remaps/backpressure_events/windows_bytes_done
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

### A3 — bounded: `--rss-gb 8`, peak RSS 7.99 GB, 5,109.6 positions/s

```
A3_BOUNDED positions=1001472 positions_per_s=5109.619 peak_rss_gb=7.994 cap_gb=8 jobs=96
```

Peak RSS is `VmHWM` from `/proc/self/status` (the kernel's number), also journaled
per second as `CONSULT_RSS`. The window set is their cache geometry: 489 T2048
windows at 2048-aligned offsets, spine = last ≤1024 bytes cut at the last `0x1E`,
mean `ctxlen` 709.3 / median 855 of 1024.

### A4 — crash and resume

```
A4_KILL exit=124 (124 = the harness's SIGTERM, the crash this gate needs)
NO_SURVIVORS work=/tmp/consult-fix/g234
A4_KILL file_bytes=27555240 last_commit_output_bytes=24419012 commits=7
A4_RESUME positions=1001472 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d truncated_bytes=3136228
A4_RESUME_IDENTICAL to_uninterrupted_run=1 positions=1001472 sha256=679984ed474889fbb324c85a6e441c49db7a5686eb17cbd1004d8e3d7a58d56d
```

The killed run had appended 27,555,240 bytes but committed 24,419,012, so
`--resume` discarded a real 3,136,228-byte torn tail, verified the committed
prefix's sha256, and finished with the uninterrupted run's exact bytes and row
count. Every launch in the harness is `setsid timeout --kill-after=10 N` and every
step asserts no `xsa consult` process of ours survived (`NO_SURVIVORS`).

## 7. Throughput, and what it is measured on

All numbers below are one process on this box with **foreign load**: four
FastGA/FastGCD jobs at ~15 cores each plus GPU training jobs, `load average 40-96`
throughout (it rose from 24 to 96 while the gates ran, which is why repeated runs
of the same command vary by ~10%). Source: `CONSULT_STATS`/`CONSULT_DONE` in the
journal. Walls include container open (`Index::open`) and the corpus window map.

| window set | spine | positions | `-j` | positions/s | peak RSS | sha256 (short) |
|---|---|---|---|---|---|---|
| T2048 x 489 (their geometry) | ≤1024, cut at `0x1E`, mean 709 | 1,001,472 | 96 | **5,109.6** | 7.99 GB | `679984ed…` |
| T2048 x 489 | same | 1,001,472 | 96 | 5,641.0 | 7.99 GB | `679984ed…` |
| T2048 x 7,849 | same | 200,704 | 16 | 2,284.0 | 7.99 GB | `16b7a268…` |
| T2048 x 489, `--rss-gb 16` | same | 1,001,472 | 96 | — | 11.36 GB | `679984ed…` |
| T1024 x 16, `--no-sep-cut` | 1024 for every position | 16,384 | 96 | **4,956.9** | 5.63 GB | `8bcd491d…` |
| T1024 x 16, `--no-sep-cut` | 1024 for every position | 16,384 | 64 | 4,533.5 | 5.63 GB | `8bcd491d…` |

For scale: the emender's receipt (`receipts/c14_cache_scale/benchmark_summary.json`,
1,034,056 positions, `jobs=128`) is **6,088.7 positions/s** with a peak RSS of
**295 GB**, at a mean `context_bytes` of **14.9** and **6.9M native queries**
(≈6.7 per position). This path answers the same 1M-position geometry at 5,109.6
positions/s with **one** query per position, mean spine 709 bytes (≈48x the rank
work per position), in **7.99 GB**. The per-position cost is therefore ~240x
lower; the wall is bounded by the query kernel, not by the stream.

Where the throughput came from (measured, `-j 32`, 1024-byte spines, pile):

| change | positions/s |
|---|---|
| as first written | 1,047 |
| cache-line-padded `Shard` (false sharing on the per-lookup counters) | 2,433 |
| + `consult_answer` drops the redundant full backward search | 3,073 |
| + a 1 GiB arena instead of 64 MiB (fewer random block decodes) | 4,177 (`-j 64`) |

Remaining headroom, and why it is not taken here: the kernel's parallel
efficiency is ~25% (`-j 64` gives ~4.2x of one core's rate), it is
memory-latency bound (`rank` walks the container's 5.9 GB of directories at
random), and the box was running foreign 60+ core workloads. Closing that gap is
deliverable B's ladder, not this stream's.

## 8. Differences from the emender python chain, and what to adapt

1. **One query per position, not 6.7.** Their `select` binary-searches the
   longest occurring spine suffix (a validity rule for a sampled corpus) and pays
   ~6.7 queries per position. `xsa consult` queries the spine once. For a
   position whose spine occurs — every position of a corpus the container covers
   — the two agree exactly. If their window sets ever point at text the container
   does not cover, their selector would shorten the spine and we would answer
   with the (absent) full one.
2. **The spine cut at `0x1E` is implemented, the backoff is not.** Same spines,
   same histograms, for every position whose full spine occurs.
3. **The row layout is ours** (§3) — a documented fixed-layout record; their
   reader adapts once (mapping in §3) or reads our layout directly.
4. **Their `censored` bit** means "the backoff search was capped at the available
   context". We have no backoff, so for any nonzero `ctxlen` their reader should
   set `censored = 1`;
5. **No python, no IPC.** Rows are assembled in the same process that queries, so
   the 121k/s assembly and the 5.9k/s orchestration walls of their chain both
   disappear from the path.

## 9. Residual risks and limits

- **Throughput on the hardest workload is only just over the bar** (§7): a full
  1024-byte spine for every position is ~50x the rank work of their measured
  workload, and the pre-existing query kernel's parallel efficiency (~25% at
  64-96 threads on a box with foreign load) is what limits it, not this stream.
- **Fixture containers cannot be built on this box for very small inputs** (the
  tape route refuses below ~26000 rows, the bin route dies in `xsa_finish` with
  "the sentinel row is missing or duplicated"), so the brute-force oracle runs
  against the real 100 GB container (gate A1) rather than a synthetic one.
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
