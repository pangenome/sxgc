# finish-decomposed: the fold rides the stitch, the verification goes parallel

Base `641e00e` (pile-week-v1), branch `finish-decomposed`. Inherited from
`binbwt-v3-finish-fast` (`a123e98`, cherry-picked as `e2abe36`): the measure-first
instrument (`PILE_FINISH_PROF` ITIMER_PROF sampler with ELF .symtab resolution,
`PILE_FINISH_REGION` wall/CPU/fault/device-read deltas, `PILE_FINISH_ACCESS`
corpus-read/page-hop counters, and the `pf_check(bool,const char*)` overload).

Producer rebuilt from this tree is `sha256 5fa842fc1907...`, i.e. the attested
`binbwt` v2 binary byte for byte: the producer side of every gate below is the
already-gated one, so what these numbers measure is the finish.

## 1. STEP 0 - the split, measured before anything was restructured

1 GB fixture, 96 threads, our own producer+finish the only jobs of ours on the
box (foreign load ~16 cores from `analysis/g1k_phased.py`, labelled; loadavg
16.8-21.0 before the run). `cmp=0`, and the counters are the charter's exactly:
`queries=473695414 verified_symbols=12820424563 seam=29271872`.

| phase | wall s | serial? | unit rate |
|---|---:|---|---|
| readiness (PFCK) | 2.21 | serial | one corpus pass |
| **sweep** | **189.37** | mixed | see below |
| m5-ef | 25.88 | mixed | 283,296,933 witnesses |
| m1-runs | 18.43 | serial | 368,036,609 runs |
| publish | 0.10 | serial | |
| **finish** | **236.0** | | |

The sweep, from `PILE_FINISH_REGION` (disjoint per band, so these neither
overlap nor double count; repair = piecewait + readband + feed):

```
piece-wait      wall=24.185  cpu=   0.654
readband        wall= 5.486  cpu=   5.577   raw_runs_ns= 14.91
repair-feed     wall=51.966  cpu=  51.746   raw_runs_ns=141.20
lce             wall=71.400  cpu=6585.518   runs_ns=   194.00
advance-witness wall=32.035  cpu=  32.152   runs_ns=    87.04
```

**The decision the charter asked for, from these numbers:** `lce` is 71.4 s wall
against 6585.5 s of thread CPU = **92.2x parallel**. The verification - the
12.82e9 verified symbols and 473.7e6 queries, i.e. `SlimFingerprint::lce`'s
hash probes plus its full-prefix direct-verify loops - **was already fanned out
per band**, and the main thread was one of its 92 workers. So "verification goes
parallel" could not pay again *as fan-out*.

What the split does show is a **schedule** defect: per band

```
read_band -> feed_raw (SERIAL) -> LCE (parallel, 92x) -> sweep.advance (SERIAL)
```

are strictly serialised into each other, so the sweep wall is the SUM of a
parallel stage and two serial stages instead of the max. Share of the finish:

| | 1 GB | share |
|---|---:|---:|
| serial state maintenance (feed 52.0 + advance 32.0 + readband 5.5) | 89.5 s | 37.9% |
| verification (parallel, 6585 s CPU) | 71.7 s | 30.4% |
| producer backpressure (piece-wait) | 24.2 s | 10.3% |
| witness/EF + m1 encode (post-sweep) | 44.3 s | 18.8% |
| readiness | 2.2 s | 0.9% |

The second measurement that decided the design: **`feed_raw` is 29.03 ns per raw
run when the pool is idle** (measured by forcing the pool to one thread,
`PILE_FINISH_POOL=1`) and 126-141 ns when 96 verification threads share the box.
Two thirds of what the region counters call "serial state maintenance" was
memory-subsystem contention with the verification, not work. The fold is cheap;
the schedule was paying for it twice.

## 2. The decomposition as landed

The operator's two kinds of work, both honoured, with one correction: the
*exactness* of the state transition was already carried by the ordered serial
thread, so the journal at band boundaries is the `BandSlot` itself - an exact
per-band buffer, not a summary.

**(a) State maintenance stays exact and serial, and rides the ordered stages.**
`feed()` (`bit6/pile_finish.cpp:2815`) is the only path that touches
`RepairStream` - one `rows` prefix, both coalescers, the seam cursor, the
dollar/padding guards - and it transits every band boundary on the calling
thread. No monoid, no cold reconstruction (the sol6b counterexample stands). The
sink is now a **template parameter** (`feed_raw`/`seamSplice`/`append`/`emitOne`,
`bit6/pile_finish.cpp:1537-1606`), so the band loop's sink inlines rather than
going through a `std::function` per run; the `std::function` overloads remain for
the m1 replay, the chunk planner and the selftests, and pass `emit` as the sink.
`Sweep::advance` (`:2570`) remains the single ordered consumer of the run stream.

**(b) Verification is a pure function of one band, so it moves off the thread.**
`dispatchLce()` (`:2768`) / `awaitLce()` (`:2776`) with `FinishPool::start()`/
`wait()` (`:2231`/`:2238`): the barrier is split so the pool can verify band k
while this thread folds band k+1, and - since the advance of band k is also
independent of band k+1's verification - the next verification is dispatched
*before* the ordered advance. Measured effect at 1 GB: exposed verification
15.2 s -> **4.29 s** of a 110.3 s verification span.

**(c) The journal at each band boundary is the slot, and it is what makes the
fan-out exact**: `BandSlot` (`:2653`) carries that band's `nbuf`, `gidx`, the two
LCE columns, its raw runs and `prevTail` - the neighbour a band's first top-LCP
query needs. Two of them, so feed(k+1) and verify(k) never share a buffer. The
query multiset, its order and its arguments are unchanged: the sol6b audit is
what licenses the move, and the counter gate is what proves it.

**(d) The flow credit moved one stage earlier, deliberately.** `consumed_band(k)`
is now declared inside `feed(k)` - after `read_stream_band` has copied the band's
raw runs into RAM and unlinked the piece, and *before* `ready_band(k+1)`. That is
what lets the producer publish k+1 during k's verification, and it is why a W=1
window cannot deadlock (consumed(k) precedes ready(k+1)). The wire shape is
untouched (one integer in one file, same names); the gates below include W=1 and
W=2 windows.

**(e) Two per-unit items from the same playbook** (they are what the serial floor
is made of): every failure path in the fold and in the verification loop is
`[[cold]] [[noreturn]]` and out of line (`pf_fail` at `:82`, `slim_fail`, and the
three `CYCLIC_SEAM_REFUSED` sites + `report()` hoisted out of
`SlimFingerprint::suffix()`/`::lce()`/`charge()`/`CorpusText::operator[]`); and
m5 stops spilling a resident witness set (`:3087`).

## 3. Gates (verbatim)

Tiny product gate on this binary - independent brute padded finite order, real
producer + real fused finish, chi and EVERY witness:

```text
BINBWT_TINY_PRODUCT_PASS cases=48 fixtures=12 real_windows=3 NUL=1 byte05=1
wrap_prefix=1 periodic=1 nested=1 cap=1,7 threads=1,3 raw_runs=brute_exact
chi=brute_exact witnesses=all_exact container=validated
```

100 MB fixture (`chi=29716349`, bank `xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`):

```text
100m: producer=0 finish=0 cmp=0   SXI2_PASS bytes=48768685 lite=1 members=6
100m: PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515
100m: queries=49845539 verified_symbols=1176027277   (identical to STEP 0)
100m sha256 b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5
     == bank sha256     finish 13.79 s   sweep 10.99 s
100m window=2: producer=0 finish=0 cmp=0    (tight flow credit, no deadlock)
100m window=1: producer=0 finish=0 cmp=0    (W=1, no deadlock)
```

1 GB fixture (`chi=283296933`, bank `pilev2-g2-1b/frag.sxi2`):

```text
1b: producer=0 finish=0 cmp=0   SXI2_PASS bytes=467814822 lite=1 members=6
1b: PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609
1b: SLIM_FP pile-finish-corpus queries=473695414 verified_symbols=12820424563
1b: SLIM_FP pile-finish-seam   verified_symbols=29271872
1b sha256 38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f
    == bank sha256
```

Counter identity is the hard gate and it is EXACT: 12,820,424,563 corpus verified
symbols + 29,271,872 seam verified + 473,695,414 queries, the same numbers the
charter quotes, with the work laid out in a different order. Two real defects
were caught by that gate while the byte gate stayed GREEN, which is the argument
for it: (i) restamping `prevTail` after the next band had been fed changed one
top-LCP query per band boundary (+571 witnesses, identical container); (ii) the
final flush's slot taking its stale pre-band `prevTail` instead of the last
emitted tail (4 verified symbols, identical container). Both are fixed at
`bit6/pile_finish.cpp:2795` and `:2860`.

Attestation (`tools/binbwt/attest.py`, the v1/v2 check set on the new roots):

```text
ATTEST_PASS binbwt-gate-1b-fdF
ATTEST_PASS binbwt-gate-100m-fdF
  sweep conservation sweep_bytes=2000001330 expected=2000001330 windows=1
  row coverage emitted_rows=1000000665 n=1000000665
  position bound peak_live_slots=2000001330 full_SA=0 persistent_positions=0
  construction counters bit-identical to v1 (comparisons=3612105756 symbols=112713502363)
```

Standing selftests, green on the final binary:

```text
SWEEP_STATE_CARRY_ORACLE_PASS streams=2048 splits=34785 NUL=1 byte05=1 ...
CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=3392 workers=4 both_coalescers=1
SEAM_SPOOL_READER_ORACLE_PASS empty=1 adversarial_orders=2 padded_flanks=1 ...
BAND_PROGRESS_READER_ORACLE_PASS trivial_bases=1 band_only=1 deferred_class=1 ...
```

## 4. Walls, before and after

1 GB / 96 threads, finish wall (the producer ran concurrently; its own wall was
98.3 s of which 60.4 s was `v2_flow_wait` on this finish, so the finish IS the
end-to-end limit):

| phase | STEP 0 | landed |
|---|---:|---:|
| readiness | 2.21 | 2.01 |
| sweep | 189.37 | **115.34** |
| m5-ef | 25.88 | **7.29** |
| m1-runs | 18.43 | 16.95 |
| publish | 0.10 | 0.09 |
| **finish** | **236.0 s** | **141.68 s** (1.67x) |

Landed sweep anatomy: piecewait 22.74 | readband 5.19 (14.10 ns/raw run) |
feed 46.38 (126.01 ns/raw run contended, **29.03 uncontended**) | advance 32.58
(88.51 ns/run) | exposed verification 4.29 of a 110.29 s verification span
(71.40 s standalone, 92x).

**Bar: finish wall <= 150 s at 1 GB/96t - MET (141.68 s). Stretch <= 80 s - NOT
MET**, and the anatomy says why: the verification's *concurrent* throughput is
now the binding term (110.3 s span vs 71.4 s standalone; the fold's traffic
costs it 39 s) and the two post-sweep stages are 24.2 s of serial work. The
serial fold is no longer the wall - 46.4 + 32.6 s of it is hidden inside the
verification window.

m5 alone: `presort_parallel()` used to sort every value-range bucket and spill
ALL of them to disc even when the whole set fit `--wit-ram-budget`, then merge
them back with preads and varint decodes (17.4 s of spill + 9.1 s of merge
decode). Buckets partition the value space in order, so sorting each bucket in
parallel and walking the buckets in range order is the identical stream: 26.9 ->
7.3 s, encode 32.1 -> 22.4 ns/witness, and the strictly-increasing assertion
still runs over every row.

### Revised pile projection (n = 1.31e12 bytes, arithmetic shown)

| term | rate | x 1310 GB |
|---|---|---|
| producer, construction + stitch (v2, unchanged) | 71.7 s/GB | 26.1 h |
| **finish own work, STEP 0** (236.0 - 24.2 pipe wait) | 211.8 s/GB | **77.1 h** |
| **finish own work, landed** (141.7 - 22.7 pipe wait) | 118.9 s/GB | **43.3 h** |
| end-to-end = max(producer, finish), STEP 0 | | 77.1 h |
| **end-to-end = max(producer, finish), landed** | | **43.3 h** |

The finish is still the end-to-end term (43.3 h vs the producer's 26.1 h), and
the honest caveat is the same one the v2 report makes about the build term: every
number above is a 1 GB rate on a *page-cache-resident* 1 GB corpus. At pile the
verification's 1.68e13 verified symbols and 6.2e11 hash probes read a 1.31 TB
text at 114 ns per corpus read for the 1 GB case (57.9e9 corpus reads); if that
becomes device reads the verification term is the projection's weakest number by
the same 77x factor the un-cached fill measured. `PILE_FINISH_POOL` exists to
re-measure that curve; at 100m the answer is 96 threads > 64 > 48 (the
verification wants every thread it can get).

## 5. Residual risks

* **The finish is now verification-throughput-bound**, not fold-bound, and the
  contention curve is the open question: the same work takes 71.4 s alone and
  110.3 s sharing the box with the fold (39 s of interference on 1 thread's
  worth of traffic). Nothing here is left to gain from moving serial stages
  around; the next lever is the LCE's 114 ns per corpus read.
* **m1 (16.95 s) is still a serial replay** of `rlebwt` at 42.01 ns/run, and it
  cannot be buffered in RAM at pile (the normalized run stream is 5 B/run =
  2.4 TB at n=1.31e12). Chunking it needs exact transducer entry state per chunk
  (the experimental `repair_chunks` planner is the only thing that has it); not
  attempted.
* **The flow credit is declared one stage earlier** than before. The argument is
  structural (the band's raw runs are in RAM and the piece is already unlinked
  before `consumed_band`), and W=1 and W=2 window gates pass byte-identically,
  but `tools/binbwt/model_sink2.py` was NOT extended for the new declaration
  point: it models the wire shape, which did not change. If the transport is ever
  re-shaped, model it first.
* `PILE_FINISH_POOL` is an env knob with no gate; the default is `--threads`.
* The sampler's phase tags are no longer disjoint (phases genuinely overlap now),
  so `PILE_FINISH_PROF_PHASE` shares are approximate; the `REGION` walls/CPUs are
  the numbers used above.
* Everything inherited from v2 stands: `--bin-level-bytes 2` is 10x worse, the
  placing pass is load-sensitive, and the file-window (non-RAM-cached)
  construction is 77x slower. `--repair-chunks` keeps its serial order and
  disables the pipeline.

## 6. Branch, reproduction, commits

Branch `finish-decomposed`, base `641e00e`. Commits: (1) `e2abe36` inherited
instrument, (2) `0e5e0d4` STEP 0 anatomy, (3) `e70eaa4` the pipeline, (4)
`a8f5269` cold paths + resident witnesses, (5) `HEAD` advance-under-verification
+ template sink, this report.

```sh
g++ -O3 -std=c++17 -pthread -rdynamic -I /home/erikg/usr/include \
    bit6/pile_finish.cpp -latomic /home/erikg/usr/lib/libsdsl.a -o pile_finish
g++ -O3 -std=c++17 -pthread -I /home/erikg/usr/include bit6/cross_lcp_merge.cpp \
    -latomic -o binbwt     # sha256 5fa842fc... = the attested v2 producer
BINBWT_BIN=./binbwt PILE_FINISH_BIN=./pile_finish python3 tools/binbwt/gate.py 100m fdF 96 4294967296
BINBWT_BIN=./binbwt PILE_FINISH_BIN=./pile_finish python3 tools/binbwt/gate.py 1b   fdF 96 4294967296
GATE_PIECE_WINDOW=1 BINBWT_BIN=./binbwt PILE_FINISH_BIN=./pile_finish python3 tools/binbwt/gate.py 100m fdW1 96 4294967296
BINBWT_TINY_ROOT=<fresh> BINBWT_BIN=./binbwt PILE_FINISH_BIN=./pile_finish python3 tools/binbwt/tiny_product.py
python3 tools/binbwt/attest.py /mnt/nvme2n1/erikg/binbwt-gate-1b-fdF /mnt/nvme2n1/erikg/binbwt-gate-100m-fdF
./pile_finish --selftest-sweep-monoid ; --selftest-repair-chunks ; --selftest-seam-spool <fresh dir>
```

Gate roots must be fresh (the producer refuses a stale root). Logs and products
under `/mnt/nvme2n1/erikg/binbwt-gate-*`, `/mnt/nvme2n1/erikg/fd-*`.
