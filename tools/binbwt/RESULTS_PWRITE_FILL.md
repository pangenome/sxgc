# pwrite-fill + auto-cap: the window write path charges no address space, and
the default live cap sizes itself

Branch `pwrite-fill`, base pile-week-v1 @ `82221bc`.

## The live failure this fixes

`/mnt/nvme2n1/erikg/sol10.log` — 10 GB, **the DEFAULT `--bin-live-cap`** (no
explicit flag), `--memory-gb 100` (PHASE_CAP_KB=104857600):

```
XSA_PLAN_V2_BIN live_cap_slots=5704253440 windows=2 window_file_gb=85.0 ram_group_gb=85.0
BIN_WINDOW index=0 b0=0 b1=105 live_slots=5578422316 file_bytes=44627378528 live_scratch_bytes=44627378528 credit=2
CROSS_LCP_FATAL bin window file mmap; the fused finish was killed after the bin producer failed
```

Two independent defects:

1. the fill wrote the window through a `MAP_SHARED` mapping of the whole file,
   so the window's 44.6 GB charged the phase's `RLIMIT_AS` on top of the 10 GB
   corpus cache and the 89 GB arena pair;
2. the derived cap priced *only* the arena pair (`85% of --memory-gb / 16 B`),
   so it could not fail before the phase did.

## 1. The write path: buffered pwrite, never a mapping

`bit6/binbwt_slice.h`:

* `BinWindow::create` (line ~340) truncates + `posix_fallocate`s the file and
  writes the header — no `mmap` anywhere; `seal` (line ~365) writes the
  completion word and `fsync`s; `close` drops the fd. The 4096 B header, the
  slot layout and every crash-refusal check in `openRead` are untouched.
* `BinFillSink` / `BinRamFillSink` (line ~300) are the fill's two destinations.
  The scatter calls `dst.put(c, slot, word)` inside the SAME inner loop on both
  arms (`fillInto`, line ~1200), so no counter and no ordering changes: each
  (bin, chunk) region is written strictly ascending, a stream is a pure append,
  a flush writes exactly the slots it holds at their absolute offset, and the
  on-disc image is byte-identical to the RAM arena. New fail-loud checks: stream
  append order, per-stream region coverage at `finish()`, write-beyond-window,
  short write.
* Staging: `bin_fill_stage_bytes_per_thread = 32 MiB` (line ~307), WATER-FILLED
  over the window's bins by `BinFillSink::view`, so every pwrite is as large as
  the budget allows and no flush can be per position. `posix_fallocate` maps the
  extents once instead of once per 8-64 KB write.
* New counters (additive, `BIN_WINDOW` + `BIN_WORK`): `win_fill_buf_bytes`,
  `win_fill_pwrites`, and `addr_bytes=0` on every window line
  (`BIN_WORK ... win_fill_buf_bytes=3221218424 win_fill_pwrites=24759`).

### Why the tuning was necessary (write rate is a function of write size)

Raw `pwrite` into page cache on this box (XFS, `/dev/nvme2n1`, 8 GiB into a
preallocated file, 256 interleaved zones; load 78-113 — a ladder run was LIVE on
the box the whole time):

```
scattered 16 KB writes: 3.3 GB/s      64 KB: 6.2 GB/s      256 KB: 8.2 GB/s
mmap dirty stores (base arm, fill-only, 1 GB fixture): ~13 GB/s
```

| staging budget | 1 GB fill (`fill_s`) | 100 MB fill |
|---|---|---|
| 4 MiB/thread (16 KB writes) | 8.57 s (12.4x the mmap arm) | 0.963 s |
| 16 MiB/thread | 2.61 s | 0.406 s |
| **32 MiB/thread (~128 KB writes)** | **2.05 s** | **0.342 s** |
| 64 MiB/thread (staging stops fitting in cache) | 2.83 s | — |

### Fill rate, before/after (median of 3, interleaved, same window, load ~106)

`--bin-probe --bin-fill-only`: the fill + the window write are the whole
measurement (`win_file_bytes` is identical on both arms: 800,004,024 B at 100 MB,
8,000,005,320 B at 1 GB — the pwrite path writes exactly the same image).

| fixture | arm | fill_s | window bytes/s | fill+seal (`win_write_s`) |
|---|---|---|---|---|
| 100 MB | base (MAP_SHARED) | 0.077 | 10.4 GB/s | 0.288 |
| 100 MB | **pwrite** | 0.378 | 2.1 GB/s | 0.583 |
| 1 GB | base (MAP_SHARED) | 0.616 | 13.0 GB/s | 2.68 |
| 1 GB | **pwrite** | 1.98 | 4.0 GB/s | 3.97 |

**Honest verdict: the write path regresses the isolated fill term by 3-5x and
the whole window-write phase (fill + seal/fsync) by 1.5-2x, and it does not
reach the >5% budget.** The gap is not tunable away: the mmap arm's stores are
asynchronous dirty-page writes (the kernel sorts and coalesces the writeback and
the writer pays nothing), while pwrite must copy every byte through the page
cache (8 GB of stores + 8 GB of copy at 100 MB→8 GB window amplification) and
then the same fsync as before. The tuning above took it from 12.4x to 3.0x on the
1 GB fixture; 24 threads measures 1.68 s (fewer chunks ⇒ larger regions ⇒ larger
writes) and 8 threads 1.70 s, but that trades scan parallelism, so the default
stays at `NT = threads` and this is reported, not hidden.

Consequence for the plan's terms: the fill term is 0.5-1% of the producer wall at
these scales (1 GB: fill 2.1 s of a 106 s producer) and it is priced by
`BIN_FILL_FIRST_NS_PER_BYTE` (the 5.238 GB/s device floor, unchanged by this
work). At the pile (266 windows at the derived cap) the fill term is the one term
that gets worse than the mmap arm measured it — see the plan's `fill_s`.

## 2. The auto cap: derived, never unbounded

`bit6/binbwt_slice.h` `BinCapTerms` (the producer, against the live `RLIMIT_AS`)
and `xsa/src/plan.rs` `bin_derive_live_cap` (the plan) — ONE formula, two
implementations, and they agree to the slot (verified below).

```
usable        = 90% x RLIMIT_AS                       (BIN_PHASE_HEADROOM_PCT)
cache         = min(n, 512 B x CROSS_POOL_SLOTS)      the corpus text CorpusText keeps in RAM
stage         = threads x 32 MiB                      the fill's pwrite staging
group         = usable - cache - stage                what the 16 B/slot arena pair may take
cap_addr      = group / 16 B
disc_budget   = 90% x scratch free - rlebwt - piece window
cap_disc      = disc_budget / (credit 2 x 8 B)
live_cap      = min(cap_addr, cap_disc, n)            (>= 1)
```

The window FILE appears only in the SCRATCH equation: with the pwrite write path
a window of any size charges zero address space. The finish is its own process
under the same cap, and its witness stream is checked against the same headroom
(it spills, so an over-cap witness stream costs disc, not feasibility).

An EXPLICIT cap that cannot fit is refused LOUD, twice: the plan marks RAM
INFEASIBLE + prints the largest cap that fits, the front door refuses before
spawning anything, and the producer refuses at startup (`CROSS_LCP_FATAL`, exit
2) — never a bad_alloc or an mmap in the middle of a window.

### The render, verbatim (10 GB, `--memory-gb 100`, 96 threads)

```
XSA_PLAN_V2_CAP live_cap_slots=5213470089 source=auto from --memory-gb 100 (bound by address space) bound_by=address space (--memory-gb): corpus cache + fill staging + 16 B/slot arena pair; MEMORY bound: 90% of --memory-gb 100 GiB (107374182400 B) = 96636764160 B usable - corpus cache 10000017258 B (min(n, 512 B x pool slots 33554432)) - fill staging 3221225472 B (96 threads x 33554432 B) = 83415521430 B / 16 B per build-group slot = 5213470089 slots; SCRATCH bound: 90% of 2532.1 GiB free = 2446963609190 B - rlebwt+pieces 17766893880 B = 2429196715310 B / (credit 2 x 8 B per slot) = 10000017258 slots; window_file_gb=77.70 = credit x cap x 8 B ON DISC (0 B of address space: the fill writes with buffered pwrite, never a mapping); the finish is its own process under the same cap: witness 21.1 GiB fits it, headroom_pct=90
XSA_PLAN_V2_BIN live_cap_slots=5213470089 windows=2 window_file_gb=77.7 ram_group_gb=77.7
XSA_PLAN_RAM FEASIBLE: RAM term = ONE BUILD GROUP ~77.7 GiB ... + the corpus cache 9.3 GiB + the fill staging 3.0 GiB = 90.0 GiB against the usable 90% of --memory-gb 100 GiB (fits)
```

The producer prints the identical arithmetic from its own `RLIMIT_AS`:

```
$ ( ulimit -v 104857600; CROSS_POOL_SLOTS=33554432 binbwt --bin-cap-print 10000017258 --threads 96 )
BIN_CAP_DERIVED n=10000017258 threads=96 rlimit_as=107374182400 usable=96636764160 cache=10000017258 stage=3221225472 group=83415521430 headroom_pct=90 cap=5213470089 windows=2
```

**1 GB / `--memory-gb 40`**: `live_cap_slots=1000000665 windows=1` (the whole
corpus, one window — the shape the banks were made with); memory bound 2328253238
slots, scratch bound 1000000665, so `min(n)` binds.
**100 MB / `--memory-gb 24`**: `live_cap_slots=100000503 windows=1` (same).
**Pile 1.31 TB / `--memory-gb 100`**: `live_cap_slots=4940890112 windows=266`
(cache is the 17.18 GB pool budget there, not n; scratch bound 26212298678
slots, so the memory side binds and the window files are 73.6 GiB of disc).

## GATES (verbatim)

Full oracle, all five passes (`c++ -O2 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic`, scratch `/mnt/nvme2n1/erikg/pwf-oracle-2`, box carrying a LIVE ladder run):

```
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
BINBWT_ORACLE_WINDOW_PASS cases=12864 fixtures=134 modes=cyclic,dollar threads=3 level_bytes=1,2 cap=1,7 credit=1,2,3 build_cap=0,5 disc=1 ram=1 file_reuse_runs=11248 window_instances=176784 window_files=88392 exact_brute_SA=1
BINBWT_ORACLE_REFUSAL mode=unlink rc=2 said=*MISSING*
BINBWT_ORACLE_REFUSAL mode=truncate rc=2 said=*TRUNCATED*
BINBWT_ORACLE_REFUSAL mode=stale rc=2 said=*STALE*
BINBWT_ORACLE_REFUSAL mode=nomagic rc=2 said=*BAD MAGIC*
BINBWT_ORACLE_REFUSAL mode=nodone rc=2 said=*INCOMPLETE*
BINBWT_ORACLE_REFUSAL_PASS refusals=5 modes=unlink,truncate,stale,nomagic,nodone control=exact_brute_SA
BINBWT_ORACLE_ADAPTIVE_PASS cases=4016 fixtures=134 modes=cyclic,dollar threads=1,8 target=1,2,3,17 depth_cap=256 level_bytes=1 cap=7,all depth_max=64 leaf_rows_max_le_target=1 over_target_leaves=0 exact_brute_SA=1 window_instances=21736
BINBWT_ORACLE_FREQ_PASS cases=6432 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all freq_sched=1 exact_brute_SA=1 multi_window_runs=2904 window_instances=33096
```

Byte identity + chi THROUGH THE SINGLE ENTRY (`xsa build --v2-pipeline`), the
DEFAULT cap (no `--bin-live-cap`), and the windowed variants:

```
GATE 100m  single  default cap  rc=0 cmp=0 sha=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5 bank_sha=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5
  V2_BIN_CAP live_cap_slots=100000503 windows=1 source=auto    V2_CHI_EXPECT_OK chi=29716349 == 29716349
GATE 1b    single  default cap  rc=0 cmp=0 sha=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f bank_sha=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f
  V2_BIN_CAP live_cap_slots=1000000665 windows=1 source=auto    V2_CHI_EXPECT_OK chi=283296933 == 283296933
GATE 100m  windowed (cap 20000000, 6 windows)  rc=0 cmp=0 sha=b4753899... (== bank)   V2_CHI_EXPECT_OK chi=29716349 == 29716349
GATE 1b    windowed (cap 250000000, 5 windows)  rc=0 cmp=0 sha=38f03eb4... (== bank)   V2_CHI_EXPECT_OK chi=283296933 == 283296933
  BIN_WINDOW index=0 b0=0 b1=59 live_slots=249361505 file_bytes=1994892040 ... addr_bytes=0
```

THE REGRESSION CASE ITSELF — 10 GB, the DEFAULT cap, under the phase ulimit that
killed the mmap, fill-only probe (`/mnt/nvme2n1/erikg/pwf-10g-default/run.log`):

```
PHASE_CAP_KB=104857600 threads=96 pool=33554432
BIN_LIVE_CAP source=auto cap=5213470089 n=10000017258 threads=96 rlimit_as=107374182400 usable=96636764160 cache=10000017258 stage=3221225472 group=83415521430 headroom_pct=90
BIN_WINDOW index=0 b0=0 b1=103 live_slots=5146052222 file_bytes=41168417776 live_scratch_bytes=41168417776 credit=2 fill_buf_bytes=3221202944 fill_pwrites=87009 addr_bytes=0
BIN_WINDOW index=1 b0=103 b1=256 live_slots=4853965036 file_bytes=38831720288 live_scratch_bytes=38831720288 credit=2 fill_buf_bytes=3221217280 fill_pwrites=48874 addr_bytes=0
BIN_WORK ... windows=2 sweep_bytes=30000051774 sweep_passes=3 ... fill_s=16.986
	Maximum resident set size (kbytes): 12870232
	Elapsed (wall clock) time: 0:49.52      Exit status: 0
```

The 41.17 GB window that failed as a 44.6 GB mapping now costs 0 bytes of address
space (peak RSS 12.9 GB = the corpus cache + the 3.2 GB staging, and the window
bytes are page cache).

Explicit-cap refusal, both doors:

```
$ xsa build --v2-pipeline ... --memory-gb 8 --bin-live-cap 900000000
XSA_PLAN_RAM INFEASIBLE: ... 16 B/slot x 900000000 group slots ... = ... DOES NOT FIT
XSA_PLAN_ALTERNATIVE the --bin-live-cap you passed cannot fit the phase budget: at --memory-gb 8 the corpus cache (1000000665 B) + fill staging (3221225472 B) + 16 B/slot arena pair leave room for 219357187 slots.
xsa: error: build: PLAN_INFEASIBLE: see XSA_PLAN_ALTERNATIVE lines above

$ ( ulimit -v 8388608; binbwt --binbwt 1b-snap.txt ... --bin-live-cap 900000000 )
CROSS_LCP_FATAL bin live cap: --bin-live-cap 900000000 slots does not fit the phase budget (RLIMIT_AS 8589934592 B): corpus cache 536870912 + fill staging 3221225472 + arena pair 14400000000 B (16 B/slot) = 18158096384 B exceeds the usable 7730941132 B (90% of the limit); 248302796 slots is the largest cap this phase can hold
producer_exit_code=2
```

## Residual risks

1. **The fill term regresses 3-5x in isolation (1.5-2x with the fsync).** Not
   tunable away; it is the price of not mapping. At the pile this is the term
   that moves: the plan's `fill_s` uses the device-floor constant, unchanged
   here, and the mmap arm's measured rate is no longer reachable.
2. **`BIN_CAP_RAISED` can now fire where it did not.** The derived cap at 10 GB
   is 5.21e9 slots (was 5.70e9). A byte-1 bin larger than the derived cap still
   raises it and the front door still FAILS LOUD (observed at 100 MB with cap
   4,194,304 < largest bin 16,803,424 — the product was still byte-identical
   before the refusal). The plan cannot predict the largest bin; a corpus with a
   >95%-one-symbol distribution needs an explicit larger cap or sharding.
3. **Staging RAM is 96 x 32 MiB = 3.0 GiB** at 96 threads, and it is in the cap
   arithmetic. It is per-thread water-filled, so small windows allocate less
   (100 MB: 800 MB = the whole window; 1 GB: 3.2 GB).
4. **16 KB pwrite rate is 3.3 GB/s on this box under load**; on a quiet box the
   numbers will move, and the 32 MiB budget was chosen on a loaded box.
5. The 10 GB end-to-end (producer + finish) was NOT run for byte identity: there
   is no 10 GB bank, and the box was carrying a live ladder run. Only the
   fill-only probe was run at that scale, under the failing phase cap.
