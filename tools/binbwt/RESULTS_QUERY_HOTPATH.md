# Query hot path: what an LF step actually paid for (2026-10-30)

Fixture: `/mnt/nvme2n1/erikg/bw256/t150m.sxi2`, a v6 lite container with
n = 150,000,002, R = 57,279,194 runs (2.62 rows/run), m1 block width 256 runs,
chi = 44,034,230, **107,133,760 bytes = 0.714 n**.

Command shape (single thread, 10 patterns):

```
XSA_TRACE=1 xsa query <container> --patterns /tmp/p10.pat -j 1
```

Accepted baseline before this work (`2bb3fb4`): **query_ms = 121.4**, mean
**19.2 ms per pattern**, locate **8.5-10.0 us per LF step**, 185-5,651 LF steps
per pattern, search 400-660 us per pattern.

## 1. The page-fault hypothesis for D1 is REFUTED, by measurement

D1 proposed that every rank touches a random ~200 B Huffman block in a 100 MB
mmap, so a page fault plus a decode dominate. Three measurements say the fault
half is simply not there:

| measurement | result |
|---|---|
| container on tmpfs (`/dev/shm`, every page resident) | 114.8 ms vs 115.0 ms on NVMe |
| major faults, `/usr/bin/time -v` | **0** |
| minor faults | 17,772 for 107 MB of file (mostly readahead), and identical on a warm page cache |
| repeat run, warm cache | identical to cold |

So the step was paying for **decode and pointer chasing**, not faults. The
reader's `madvise(MADV_WILLNEED)` at open was already doing its job.

## 2. What the step paid for (new XSA_TRACE counters)

Counters added to the kernel, keyed to the same run (`TRACE_PATTERN
... decodes= runs_decoded= scan_of= scan_cum=`), pattern 6 of p10, 5,651 steps:

| quantity | before |
|---|---|
| m1 block decodes per LF step | **2.04** |
| runs decoded per step | **522** |
| runs walked in `run_of`'s scan per step | 129 |
| runs walked in `cum_before`'s scan per step | 396 |
| warm cost of one 256-run block decode | **2.33 us** (9.1 ns/run; `XSA_M1_AUDIT_FULL=1` decodes all 223,747 blocks in 521.4 ms) |
| per-run `run()` probes per step (each a div/mod by a non-constant block width plus a cache-slot check) | 525 |

Arithmetic: 522 runs decoded x 9.1 ns = 4.75 us, plus 525 probes x ~6 ns = 3.2 us,
plus the metadata bisects => the observed 8.5-10 us per step. **A step that
decodes ~2 blocks of 256 runs cannot be made cheap by changing how those bytes
are fetched**; the decode itself is 2.33 us, and C1 asks for <= 1 us per step.

### What was tried and rejected

1. **pread into a slot pool / madvise(WILLNEED) at open** — the readahead is
   already at open (`TRACE stage=lite.prefetch`), faults are zero, tmpfs is not
   faster. There is nothing to win: the bytes are free, the decode is not.
2. **Prefetching the next block** — the LF walk is a dependent chain
   (`pos -> run_of(pos) -> LF -> pos'`), so there is no next address to prefetch
   until the current step finishes; and the cost is decode, not latency.
3. **A faster per-step cache** — a 4096-slot direct-mapped block cache against
   223,747 blocks cannot help a random walk (hit rate ~2%).
4. **Denser container-side sampling (candidate 4), priced and NOT implemented**:
   a per-symbol sampled cumulative array at stride 8 or 16 needs n/8 or n/16
   entries; at 8 B/entry that is **1.00 n / 0.50 n** of extra container, against
   0.036 n of headroom under the 0.75 n budget. m13 already IS that structure
   (occurrence stride 256, run gap 1024) and already costs **0.197 n**; making it
   4x denser (run gap 256) would add ~0.6 n. Unaffordable in the container at any
   useful stride, which is exactly why the fix went into RAM instead.

## 3. What landed

The reader decodes the m1 run stream **exactly once at open**, in parallel over a
partition of the block range (`par_chunks_mut` on the columns, zipped, so
disjointness is the type system's problem), into three per-run RAM columns
(`xsa/src/qlite.rs`, `Index::materialise`):

| column | size | what it answers |
|---|---|---|
| `heads[run]` | 1 B/run | the run's symbol |
| `runs[run] = len | seen << 32` | 8 B/run | run length **and** rows of `heads[run]` before the run - the LF step's rank, in the same word the scan reads |
| `dir[j] = (start row, run)` every 256 rows | 16 B / 256 rows | `run_of` in ONE load |

An LF step is now: one directory load, a ~50-run scan over `lens`, one `seen`
read. It decodes nothing. `search`'s `rank` for an arbitrary pattern byte still
uses m13 (it needs a symbol that is not the run's head), scanning the RAM column.

The open-time audit got **stronger for free**: all 223,747 blocks must sum to
their m12 span (was 1,024 sampled), the per-window row totals must equal the m1 C
table symbol by symbol, the window run counts must sum to R, and the row
directory build fails loud if the rows ever run out before the last run.

Also: `MADV_HUGEPAGE` on the columns (THP is in `madvise` mode on this box), with
the range aligned to a page because a `Vec` starts 16 B into its mapping - worth
0.92 -> 0.80 us/step on its own.

## 4. Result (same box, same command)

| metric | before | after |
|---|---|---|
| query_ms, p10, -j 1 | 121.4 | **3.1** |
| mean per-pattern total (p10) | 19,247 us | **1,365 us** |
| per-LF-step (locate wall / steps; all 10 patterns) | 8.5-10.0 us | **0.30-0.81 us** |
| search_us per pattern (24-byte patterns, 48 rank calls) | 400-660 | 84-170 |
| p100, -j 1 | see below | 474.7 ms (574/574 positions) |
| p100, -j 32 wall | see below | **0.53 s** including the open (RSS 540 MB) |
| open (`run-columns`) | 8.0 ms (1024-block audit) | 76 ms on 256 threads, 82 ms on 32 |
| container bytes | 107,133,760 (0.714 n) | **unchanged** |
| chi | 44,034,230 | **unchanged** |
| positions/counts vs `tools/binbwt/oracle.tsv` | 574/574 | **574/574, 0 missing, 0 extra** |

Cost of the design: the columns are **9 B/run = 3.44 n of RAM** here (515 MB;
19 GB at a 10 GB pile, ~210 GB at a 90 GB text), and the open decodes the whole
run stream once (67-82 ms at 150 MB on 32-256 threads). The reader refuses, in
words, when `MemAvailable` cannot hold 9 B/run. **This does not scale**: it is a
measured stepping stone that establishes the achievable rank cost (0.49 us per
LF step) and the exactness baseline, not the final shape.

## 5. D2: the walk is long because anchors are RUN-indexed, not because of a bug

m11 stores the tail SA of every 1024th **run** (`if (idx % 1024 == 0)` in
`bit6/xsa_finish.cpp`), and `locate_steps` stops when it lands on the last row of
an anchored run. A run is 2.62 rows here, so the anchor cadence is one anchored
row per ~2,680 rows, and a walk that starts at a random row halts after a
**geometric number of steps with mean 1024 x 2.62 = 2,683**:

* measured mean steps per walk (p10) = **2,321**, p100 low-count mean = 2,774,
  max 10,981 across 574 walks. P(walk > 10,981) = exp(-10,981/2,680) = 1.7%,
  so ~9 of 574 walks above that is exactly what the geometric law predicts.
* so the walk is NOT crossing runs incorrectly, and the nearest-anchor
  selection is right; the stride is simply expressed in runs while the walk is
  indexed in rows. Dividing by rows/run (2.62) explains all of the excess.

Denser anchors would cut the step count linearly, at a container price:
row-indexed anchors every 256 rows cost n/256 x 8 B = **4.7 MB = 0.031 n** here
(0.714 n -> 0.745 n, inside the 0.75 n budget) and would take the mean walk to
~256 steps, i.e. ~0.3 ms per pattern. It is NOT implemented here (it changes what
m11 means, and C1/C2 are already met without it); it is the next lever if a
future criterion asks for 0.5 ms per pattern.

## 6. Can the columns live in the container instead (mmap, lazy, no eager decode)?

The 3.44 n RAM column is not the answer at 10 GB, let alone 90 GB. Two container
shapes were priced and one was measured.

### 6.1 The byte accounting (this fixture, runs/byte = 0.382)

| column | per run | per n | note |
|---|---|---|---|
| heads (symbol) | 1 B | **0.382 n** | replaces m1's Huffman head stream |
| lengths, byte + escape | ~1 B | **0.382 n** | replaces m1's gamma length stream |
| lengths / rank, fixed u32 | 4 B | 1.527 n | |
| **per-run rank (`seen`)** | 4 B | **1.527 n** | the one thing that makes a step ONE read |
| sampled rank, every 32 runs (m13-shaped) | | ~0.19 n | today's m13 is 0.197 n at a 1024-run gap |

m1 is 0.367 n today, so "heads + lengths in the container" is
0.714 - 0.367 + 0.764 = **1.111 n** of container for a reader that could address
any run with one load - inside a 1.25 n cap, and `open` stays mmap-only.

### 6.2 But that shape does not reach C1, and this was MEASURED, not estimated

With heads + lengths in the container there is no per-run rank value, so
`cum_before(head[run], run)` has to go through the m13 directory plus a scan.
Measured with exactly that substitution in `locate_steps` (and with every byte
still in RAM, i.e. strictly OPTIMISTIC against a paged container):

    lf_us_per_step = 2.035   worst single pattern = 9.310   mean per pattern = 5,353 us

against C1 <= 1 us and C2 <= 2,000 us. The cost is the m13 per-symbol lookup
(2-3 dependent random reads in the 29.5 MB entry array) plus a scan bounded only
by the container's own 1024-run gap. A 32-run sampling cadence removes most of
the scan but not the lookup, so a container-side rank lands at ~1.5 us/step -
still over C1 - while costing another ~0.19 n.

**Conclusion: at <= 1.25 n there is no container shape that gives a one-read
rank.** A one-read rank costs 1.527 n by itself (2.68 n container total, or
~1.5 n with 12-bit deltas from a 32-run sample). The economics only close if the
walk itself gets ~10x shorter, which is section 5's row-anchored m11
(+0.031 n, mean walk 2,321 -> ~256 steps): with anchors at every 256 ROWS, even
the 2.0 us container-side step gives ~0.5 ms per pattern, and only C1's per-step
clause would still be out of reach.

So the next step - deliberately not started here, so nothing is left half-
written - is a format change in this order:
  1. m11 re-anchored by ROW (stride 256): +0.031 n, 10x fewer steps. Reader side
     only, no new member, m11's meaning changes from runs to rows.
  2. heads + lengths as container members replacing the m1 payload: 1.111 n,
     `run_of` becomes one directory load plus a <=100-byte scan, still lazily
     paged.
  3. only if C1 must hold per step at 90 GB: a per-run rank column (deltas from a
     32-run sample) at +~0.5 n, i.e. ~1.6 n of container.
