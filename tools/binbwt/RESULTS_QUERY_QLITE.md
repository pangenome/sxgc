# `xsa query` on a v6 lite container: what it costs now (2026-10-29)

Reader: `xsa/src/qlite.rs` (native SXI2 v6 lite kernel, one reader chosen by
magic). Everything below is measured on this host, walls labelled with what else
was running.

Containers:

| container | n | R | file | members |
|---|---:|---:|---:|---|
| `/mnt/nvme2n1/erikg/r10g-m96/pile-10b-snap.sxi2` | 10,000,017,259 | 3,474,889,847 | 4.51 GB | m1,m4,m5,m7,m10,m11 |
| `/mnt/nvme2n1/erikg/wip-lce150/t150m.sxi2` | 150,000,002 | 57,279,194 | 73 MB | m1,m4,m5,m7,m10,m11 |

Oracles: `tools/binbwt/oracle.tsv` (100 patterns, 574 positions) and
`/tmp/g10/oracle.tsv` (40 patterns). The 150 MB oracle was independently
re-verified against the corpus itself (`/dev/shm/t150m.txt`): 574/574 positions
land on the pattern, 100/100 counts agree, so it is an oracle and not a copy of
our own answer.

## Gates

```
tools/binbwt/gate_qlite_150m.sh   -> GATE Q1 PASS   COUNTS 100/100 exact   POSITIONS 574/574 exact
tools/binbwt/gate_qlite_10g.sh    -> GATE Q5 PASS   COUNTS 20/20 exact    POSITIONS 20/20 exact
tools/binbwt/gate_qlite_cli.sh    -> GATE Q6 PASS   11/11 checks
```

Q1/Q5 use exact multiset comparison of every reported position, not just counts.

## Startup (Q2)

```
QLITE_OPEN container=…/pile-10b-snap.sxi2 format=sxi2-v6-lite magic=SXI2 version=6
           mmap_ms=0.05 decoded_bytes=0 stages=none n=10000017259
           runs=3474889847 members=m1,m4,m5,m7,m10,m11 end_symbol=none
```

Open is mmap + header + 64+40·6-byte directory + header CRC + the 256-byte
remap. **0.05 ms**, down from 405 s. Nothing in the run stream, the chi set or
the anchors is touched, so `decoded_bytes=0 stages=none`.

What replaces the 405 s (was `sxi::Container::open` twice: two full run
decodes, two 27.8 GB LF-start arrays, a 1.29 GB chi decode into `io::sink()`,
and a 256-way `cruns` transpose):

```
QLITE_BUILD runs=3474889847 decoded_bytes=3185958861
   stages=[m1-runs=24479.5ms rank-dir=4634.6ms run-starts=264.5ms run-dir=332.2ms m11-anchors=22.4ms]
   build_ms=29733.1 threads=32
```

### The remaining 24 s is not laziness we skipped, it is the format

`m1` is **one** Huffman bit stream (run symbols, 3.19 GB / 308.3M bits) and
**one** gamma bit stream (run lengths, 1.10 GB / 109.7M bits), with no per-run
alignment and no restart directory, so run *i* is unreachable without decoding
runs 0..*i*. A query needs `rank` at arbitrary rows and an LF walk over
arbitrary rows, so a cold query needs the whole run stream. Parallelising an
entropy decode of this shape is not available (the two streams already decode
concurrently on two threads; splitting either one requires a resynchronisation
point that the writer does not emit).

Measured decode rates (so the headroom is explicit, not hand-waved):
`-j 1` build = 27,661 ms of `m1-runs`, `-j 32` = 20,929 ms → serial cost of both
streams = 27.7 s, concurrent = max of them = 20.9 s, i.e. **gamma ≈ 6.8 s (1.96 ns/run)**
and **Huffman ≈ 20.9 s (6.0 ns/run, ~17 cycles/run on 3.0 GHz EPYC 7713)**.
A hand-tuned 12-bit-table Huffman loop should reach 3–5 cycles/run, which would
put the whole build near 12 s. That is the entire remaining headroom on Q2 and
it does not change the conclusion: a format whose only random access path is a
full sequential decode cannot answer in 1 s.

## Per pattern (Q3)

Single pattern, fresh process, `-j 32`, box under load average 15–27 from an
unrelated shell (labelled):

```
TRACE_PATTERN id=0 len=24 occ=1 search_us=67.3 locate_us=3330.6 steps=4540 max_steps=4540 total_us=3397.9
```

- **search 67 µs** against the 178 µs budget: met.
- **locate 3331 µs — 3.3× the 1.1 ms budget.** Not met, and the reason is
  structural rather than a missed constant factor:

  * The LF walk must terminate on the *tail row of an anchored run*, and m11
    anchors every 1024th run, which at pile scale is one anchor per ~2950 text
    positions. So a locate is **2368–4540 LF steps** (measured over all 574
    fixture positions and 20 pile positions: mean 2774 at 150 MB, mean 2368 /
    max 6307 at 10 GB). The legacy reader's `locate_us=1121.6` figure came from
    the `--trace` code path, which stops at *any* run tail (~3 steps) and then
    cannot re-anchor; it is not the same walk.
  * Each step is a dependent chain of three cold random accesses (64-position
    run directory → run length → LF start) over ~50 GB of derived arrays.
    Measured 0.73–0.98 µs per cold step; the *same* walk replayed warm costs
    0.10 µs/step, i.e. 85–90 % of a locate is memory latency that cannot be
    overlapped because the chain is serial.
  * Box context: 2× EPYC 7713 sockets, 2 NUMA nodes, remote distance 32 vs 10,
    THP=`madvise`. A random load here is 36 ns (1 GB working set) to 55 ns
    (48 GB) with local placement; a single query thread on a 32-thread-built
    50 GB working set pays remote-node latency as well.

  Consequences for the budget: 20 low-count patterns in one process amortise
  that latency across threads — mean 2176 µs at `-j 32`, and the 150 MB fixture
  reaches mean 1132 µs at `-j 16`. A single cold pattern cannot.

## Parallelism (Q4)

20 low-count patterns (`/tmp/g10_low.fa`, all oracle count 1), 10 GB container,
identical output at both widths:

```
-j  1 : build 40838 ms  query_ms=18.9  low_count_mean_us=7265.4 max=17556.4   WALL 45.30 s
-j 32 : build 25840 ms  query_ms= 5.9  low_count_mean_us=2210.4 max= 5345.9   WALL 29.79 s
```

Wall scales 1.52×; query-only wall scales 3.2×; per-pattern scales 3.3×. The
wall is capped by the serial m1 decode, which is why end-to-end speedup is 1.5×
and not 30×: 40.8 s of the 45.3 s (`-j 1`) and 25.8 s of the 29.8 s (`-j 32`) is
build, of which 27.7 s / 20.9 s is the single-threaded Huffman stream.

## Memory

RSS 51.8 GB for the 10 GB container: `rlen` 13.9 GB + `lfstart` 27.8 GB +
`heads` 3.5 GB + `rbase` (rank blocks, 4096 runs × 256 symbols × 8 B) 1.7 GB +
`dir` (16 B per 64 text positions) 2.5 GB + `blk` 0.43 GB. That is 5.2× the
container, versus ~7× (70 GB) for the legacy open path, and it is all
*per-run or per-position-class* structure — no per-suffix position column, no
per-position text.
