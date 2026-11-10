# Stateless query core

The new path is `Index::open_stateless(Reader)` plus a caller-owned
`RefCell<WorkerArena>`. `stateless_count(index, arena, query)` accepts original
bytes. `stateless_histogram(index, arena, query, corpus_tail)` returns 256
next-byte counts and structural END separately. The tail must contain the last
`min(1024, text_bytes)` bytes of the exact indexed text for correct EOF answers. Empty histograms follow consult's
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

Q1 scaling table (verbatim):

| Threads | Wall seconds | Positions/s | Wall scaling | Efficiency/thread |
|---:|---:|---:|---:|---:|
| 1 | 99.162097 | 41.306 | 1.000x | 100.00% |
| 8 | 14.845541 | 275.908 | 6.680x | 83.49% |
| 24 | 5.277033 | 776.194 | 18.791x | 78.30% |
| 48 | 2.674794 | 1531.333 | 37.073x | 77.23% |
| 96 | 1.777656 | 2304.158 | 55.783x | 58.11% |

Limiting resource: repeated scalar Huffman/gamma block decoding. At one worker,
92.887 of 99.162 query seconds are inside `decode_block`; at 96 workers its
summed wall is 146.636 seconds versus 152.803 process user CPU seconds. Every run
performs exactly 8,950,584 block decodes and has checksum 241315550719. The
high absolute cost is explicit: about 2,185 full block decodes per position.
There are no shared cache hits to amortize this cost.

Scaling bends above 48 workers: doubling to 96 yields only 1.504x more
throughput. The measured maximum is **2304.158 positions/s, 55.783x serial,
58.11% per-thread efficiency**. Near-linear scaling to 96 was **not achieved**.
The decoder consumes the work; why its per-decode time rises at 96 cannot be
uniquely attributed to frequency, SMT placement, cache/memory latency or NUMA
without hardware counters. `perf stat -e cycles,instructions true` was denied
(`perf_event_paranoid=4`); no claim of measured bandwidth or TLB saturation is
made. This hardware attribution remains an explicit Q1 limitation. There are
zero major faults at 8–96 workers, and the mapped core issues no read/prefetch
syscalls during queries, so disk page faults and read syscall rate are not the
observed throughput ceiling. Pure scalar decoding remains expensive even after
shared-cache contention is removed; a future LCP/tree layer must reduce rank work.

Method: 4096 fixed positions, one complete next-byte histogram each, raw rolling
8-byte contexts, spread over sixteen corpus windows. This is the permitted
cheaper spine, not a claimed 1024-byte throughput result. All thread counts
answer exactly the same logical positions. Walls include worker creation and
first-touch faults but exclude index open, correctness spot checks, the RSS
snapshot, and thread teardown. Raw logs and CPU/fault rusage are committed under
`gates/stateless_evidence/`. Full process walls including open are in scaling.json.
Single trial per level, sequential 1/8/24/48/96, no affinity, warmed kernel page
cache after the first trial. Host: dual EPYC 7713, 128 physical cores/256 logical
CPUs, acpi-cpufreq schedutil, foreign load roughly 14–18. Short 96-worker wall and
host load limit precision. Reproduce with:

```sh
python3 gates/stateless_scaling.py xsa/target/release/xsa \
  /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 \
  /mnt/nvme2n1/erikg/pile.txt /tmp/stateless-scaling 4096 8
```


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


## Q3 — resident memory

At 96 workers, the follow-up run (same 4096 positions, 8-byte contexts) reports:

```
STATELESS_MEMORY workers=96 anonymous_before_bytes=299008 anonymous_live_bytes=3121152 incremental_anonymous_per_worker_bytes=29397 query_wall_seconds=1.788914
STATELESS_WORKER_RSS arena_peak_bytes=8192 stack_peak_max_bytes=12288 stack_plus_arena_peak_max_bytes=20480 heap_pair_capacity_bytes=4096 context_heap_bytes=8
VmHWM: 7886564 kB
```

Peak decoded arena RSS is **8 KiB per worker**; the largest resident worker stack
is **12 KiB**, giving **20 KiB peak stack+arena per worker**. Stack residence is
read from that fresh thread's guarded VMA after queries, before thread exit or
retirement; anonymous pages were not swapped (time reports zero swaps). The
worker also has a bounded 4096-byte pairs allocation and 8-byte transient context
allocation in this benchmark (1024-byte context in the target workload). Those
heap pages and allocator overhead do not have exclusive per-thread RSS accounting;
the aggregate live anonymous increment averages **29,397 bytes/worker**. This is
an average, not a fabricated per-thread VmHWM. The benchmark-only smaps scan runs
after the timed query and introduces its own small transient IO buffer.

Total process peak at 96 workers is **7,886,564 KiB = 8,075,841,536 bytes =
7.521 GiB** by `/proc/self/status` VmHWM. GNU time separately reports 7,769,572
KiB ru_maxrss; these kernel accounting interfaces differ, so the larger VmHWM is
used here, consistently with consult's prior gates. Raw output is Q3-96.log.
The Q1 96-worker run was 7,885,872 KiB before stack diagnostics were added.

Total is bounded by the finite immutable mapping, fixed metadata, and bounded
private storage per worker, independent of positions already answered. It is
**not an 8-GiB service cap**: the container alone can eventually occupy about
58.8 GiB when fully resident. Consult adds a bounded 64-MiB corpus window;
the scaling harness maps the full corpus but only touches selected windows.
Retirement drops private scratch pages; kernel residency/reclaim of shared input
pages is left to the OS. Do not infer the total's asymptotic bound from this small
set's 7.521-GiB observed peak.

The Q3 diagnostic change only adds post-query stack attribution; the timed core
is unchanged. Its repeated 96-worker rate is 2289.657 positions/s, within 0.7% of
Q1. After this change all 85 release tests, including the external-container
test, pass again.

## Q4 — delivery receipt

Gate commits were pushed directly to origin/main without force, fetched back,
and compared with local HEAD after each gate; local main was fast-forwarded:

- Q2: `eecff24ff08b2cb4d14320373b30878b2927da3a`
- Q1: `5b4fa22b451455fdec31c71af9cd0d9be3149ee6`
- Q3: `1fc94e76a1790cbd468d23ac175fa7c4ed4a0072`

Before this receipt commit, `git status --porcelain=v1` was empty in the isolated
`/home/erikg/sxgc-stateless` worktree, `git diff --check` passed, and HEAD and
origin/main both equalled the Q3 SHA. The receipt commit is also pushed and the
final equality/clean-state check is reported in the task result.

The original `/home/erikg/sxgc` main worktree had a pre-existing modification to
`NOTE_FROM_EMENDER_EXTRACTION_PERFORMANCE.md` at task start. It remains preserved;
that worktree is not claimed clean. No staged changes remain in either worktree.
Q1's 96-worker near-linear target and unique hardware attribution remain unmet;
the measured decoder ceiling and limitations above are the result, not a promise
that a shared-nothing design alone eliminates scalar decode cost.
