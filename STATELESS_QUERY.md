# Stateless query core

The new path is `Index::open_stateless(Reader)` plus a caller-owned
`RefCell<WorkerArena>`. `stateless_count(index, arena, query)` accepts original
bytes. `stateless_histogram(index, arena, query, corpus_tail)` returns 256
next-byte counts and structural END separately. Empty histograms follow consult's
empty-context convention. `Index::worker_ctx(arena)` also exposes the existing
rank/search navigation to layers that need several operations per query.

`xsa consult --stateless` uses this core; ordinary consult retains its compatible
shared-cache mode. `xsa stateless-bench CONTAINER CORPUS THREADS POSITIONS [LEFT]`
measures the new core with no shared decoded cache allocated. No cache opt-in is
needed for the new API. Do not call legacy `ctx()` on a stateless index.

## Memory law

The SXI mapping, C table, Huffman/gamma tables and directories are shared and
immutable. No query writes an atomic, obtains a mutex, issues pread, runs an RSS
governor, or changes the shared mapping's advice. Payload bytes are decoded
directly from the read-only mapping. The kernel page cache backs the mapping.

Each worker owns one anonymous mmap scratch arena, rounded to pages:
`round_up(round_up(block_runs, 4) + 4*block_runs, 4096)` bytes. For this
container's 1024-run blocks it is 8192 bytes. It retains only the current decode,
with no LRU or replacement policy. All counters are private. `retire()` invalidates
the decode and calls `MADV_DONTNEED` on private anonymous pages; Drop does the same.
It never drops shared file pages. A worker must exclusively own its RefCell;
sharing it across threads is forbidden by Rust's type system. Reuse it across
queries rather than allocating per position.

Additional worker state is a bounded context (at most 1024 bytes), a 256-element
histogram, bounded candidate/pair vectors, navigation counters and thread stack.
Consult's existing bounded queues and ordered output remain in effect.

Total RSS is bounded by the mapped container size plus the corpus mapping/window,
fixed index metadata and O(workers) scratch/stacks/queues. It is **not** bounded
by the old `--rss-gb` governor in stateless mode: that mode announces this explicitly
and skips the producer's governor-dependent RSS throttle. This distinction matters
when the entire file's pages eventually become resident. Benchmark maps the full
corpus virtually but touches only its selected windows. Query core inputs can use
bounded corpus windows, as consult does. The tree layer must budget immutable
mapping residency separately from private scratch.

## Measurement

Pending measured gate results.

## Tree-layer guidance

Keep persisted LCP and children/edge members immutable and mapped. Pass a worker
arena into traversal; put temporary frontier, decode storage and counters there,
with explicit maximum sizes. Do not put a shared memoization map, hit counter,
shard mutex or RSS governor in traversal. Never retire or advise a shared mapping
from a query. A service requiring a hard RSS limit should arrange mapping windows
outside the query core, with lifetimes that cannot invalidate another worker's
input. Open and audit before sharing the index. Queries must return owned results
or caller-buffer results, never arena slices surviving retirement.

The requested `CHI_TRANSITIONS.md` was absent from origin/main at the starting
revision; CONSULT.md, bit6/SXI_FORMAT.md and the reader were inspected. No
`contact_supervisor` tool was available in the runtime tool catalog.

## Q2 — correctness and determinism (verbatim)

```
Q2 query "the " = 704942163; "Harper Reed" = 55; " function " = 4785152: unchanged.
Q2 consult 35-window determinism sha256 = 418edf87b250243d21020ed1e1126b766fb3444aa969b68c81451d73ea37a009: unchanged (prior saved rows, current compatibility j=8, stateless j=1 and j=8; --strict).
```

The small set is committed as `gates/stateless_evidence/windows.jsonl`. Its
35 contexts cover lengths 1–1024, separators, and text EOF. Prior rows were
`/tmp/consult-fix/a1/a1-sep.rows`, already present before this change. All four
files are 12,459 bytes. `cargo test --release -- --include-ignored` passes all
85 tests, including the explicit 100GB count/histogram/retirement test. That test
compares full histogram counts against compatibility navigation and verifies
private pages read as zero after retirement and that shared cache stats stay zero.
