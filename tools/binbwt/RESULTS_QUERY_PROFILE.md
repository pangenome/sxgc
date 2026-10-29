# Where `xsa query` spends its time (v6-lite containers)

Measured 2026-10-29 on the pile-week-v1 line. Container: SXI2 **v6 lite**
(`n=10000017259`, `R=3474889847`, members `m1,m4,m5,m7,m10,m11`, 4.51 GB).
Fixture: `/mnt/nvme2n1/erikg/wip-lce150/t150m.sxi2`
(`n=150000002`, `R=57279194`, 73 MB) — the same container shape at 1/60.6 scale.

`perf record` is **not available on this host** (`kernel.perf_event_paranoid=4`,
no sudo, `perf stat` refuses even `cycles`). Substituted with (a) wall-clock
stage timers that are now permanent and env-gated (`XSA_TRACE=1`, functions
`trace_ms`/`trace_since`, `xsa/src/main.rs`), and (b) RSS + CPU from
`/usr/bin/time -v`. The stage timers are strictly more specific than a
sampling profile here: every stage below is a named function.

## Stage table

`XSA_TRACE=1 xsa query --ri4 <container> --patterns <1x24-mer>`, one thread,
nothing else on the box except this editor and one background shell.

| stage (function) | t150m wall | share | x60.6 → 10 GB |
|---|---:|---:|---:|
| `sxi.member-crc` — `Container::open`, CRC32 of **every byte of every member** | 198.7 ms | 3.2% | ~12 s |
| `sxi.runs-decode` — `sxi2::runs`, Huffman heads + gamma lengths, **full R decode #1** | 1794.9 ms | 28.8% | ~109 s |
| `sxi.phi-or-anchors` — `LiteAnchors::load` (27 MB, 3.4M u64 + sort) | 1.6 ms | 0.0% | ~0.1 s |
| `sxi.lf-starts` — `LfMap::load`, materialises `Vec<u64>` of **R LF starts** (458 MB @150 MB, 27.8 GB @10 GB) | 456.5 ms | 7.3% | ~28 s |
| `sxi.anchors` — `Container::anchors` (m4, empty) | 0.0 ms | 0.0% | 0 |
| `sxi.chi-decode` — `write_chi` into `io::sink()`: **decodes 2.68e9 EF chi values on every open, used by nothing** | 909.4 ms | 14.6% | ~55 s |
| `ri4.runs-decode` — `Ri4::load_validated`, **full R decode #2 of the same stream** | 1795.4 ms | 28.8% | ~109 s |
| `ri4.run-start-blocks` — run-start samples every 64 runs | 61.6 ms | 1.0% | ~4 s |
| `ri4.cruns-by-symbol` — 3.47e9 appends into 256 `Vec<u64>` (`cruns`), scattered writes over 27.8 GB | 625.6 ms | 10.0% | ~38 s |
| `ri4.lf-starts` — `LfMap::load` **again**, second `Vec<u64>` of R LF starts | 401.6 ms | 6.4% | ~24 s |
| **open total** | **6243 ms** | | **~378 s** (measured 385.9 s) |
| `TRACE_PATTERN` search (24 rank steps) | 106.5 µs | | ~1 s at 10 GB (measured) |
| `TRACE_PATTERN` locate (1 occurrence, LF walk) | 1681.8 µs | | (same walk + cache misses) |

`Maximum resident set size` on t150m: 1.258 GB for a 73 MB container (17x) —
on the 10 GB container the same code materialises ~70 GB of derived arrays.

## What the numbers say

1. **The fixed cost is not phi.** The v6 lite container has no m8, so nothing
   rebuilds phi. The fixed cost is the *legacy reader's open path*:
   `sxi::Container::open` is a **validation** entry point that the query path
   reuses as a **loader**, and it (i) CRCs every byte of the file, (ii) decodes
   the whole run stream, (iii) materialises the whole LF-start array, (iv)
   decodes the whole chi set into a sink. `Ri4::load_validated` then repeats
   (ii) and (iii) and adds the `cruns` transpose.
2. Therefore the 385 s is ~**2x run-stream decode + 2x LF-start materialisation
   + 1x chi decode + 1x cruns transpose + 1x full-file CRC**, all of which are
   *derived* data the query either does not need or needs in a far smaller
   form. The run stream itself is only ~3.19 GB (7.33 bits/run).
3. The ~1 s **per pattern** is the same data viewed the other way: `rank()`
   binary-searches 8-byte symbols inside `cruns` (27.8 GB of vectors) and
   `lf()` binary-searches a 434 MB run-start block array; every probe is a
   cache miss, and a locate is ~3e3 such LF steps (bit6/sxi_logs/skiplist-locate).
   It is not one pass over the runs per pattern — it is 24 rank steps and a
   short LF walk, each of which costs microseconds of cache-miss latency
   because the structures they touch are tens of gigabytes.

## Consequences for the query path

* Startup must be **open + mmap only** for the members the query touches
  (`m1`, `m5`, `m10`, `m11`): no full-file CRC, no chi decode, no phi, and no
  second decode.
* The derived structures the query *does* need (run starts, per-run
  own-symbol cumulative counts) must be built **once, sequentially, cache
  friendly** — never as 256 scattered `Vec<u64>` appends.
* Pattern input and output should stop pretending to be read mapping: the
  index answers `count` and `position` for arbitrary byte strings.
