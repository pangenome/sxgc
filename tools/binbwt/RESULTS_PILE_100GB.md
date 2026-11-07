# The Pile, first 100 GB: built and verified (2026-11-05)

Corpus: `/mnt/nvme2n1/erikg/pile-head100.txt`, exactly 100,000,000,000 bytes of raw `pile.txt`
(no document framing, no sidecars, raw tape route, virtual END).
Container: `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2`, 63,163,489,696 B.

```
XSA_BUILD_DONE chi=24,185,339,204 n=100,000,000,001 pipeline=v2tape
container      63,163,489,696 B   = 0.632 n
wall           12:35:10           (order-tape 12:21:46, emit-tape 12:21:42, finish 12:35:10)
waves          102              ties 32,442,986,338
peak RSS       order-tape 108.7 GB, finish 32.2 GB (finish anon 21.8 GB of 34.0 GB sampled)
peak scratch   78,880,730,679 B   = 0.789 n
```

## What this corrects
- **Scratch is not 2.2 n at scale.** 2.177-2.2 n was measured at 10 GB, where fixed overheads dominate.
  At 100 GB the concurrent band window gives 0.789 n. Pile projection: ~1.03 TB, not ~2.9 TB.
- **The wall is 1.6x Wikipedia per byte, not 3x.** 12.6 h here versus 8 h for 100 GB of enwiki.
- **The Pile is denser in distinct contexts:** chi/n = 0.242 vs enwiki 0.156 (+55%). Container 0.632 n
  vs enwiki 0.423 n. Corpus character, not size.
- chi per run = 24.18e9 / 31.38e9 = 0.771, matching enwiki's 0.78: the finish's per-run cost is stable.

## Pile projection (1.31 TB)
scratch ~1.03 TB, container ~830 GB, wall ~7 days, chi ~317 billion.

## Not yet done on this container
Sampled stream == BWT(SA), closure-0, T3 checker, and query positions against brute force ON PILE DATA.
Until those run, this is a build certificate, not a search certificate.

# Search certificate (2026-11-06)

The build certificate above ends with "not yet done: query positions against
brute force on Pile data". This is that, and it corrects two things at once: the
reader could not locate on this container at all, and the count it *could* report
was being compared against a tool that undercounts.

Container under test: `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2`,
63,163,489,696 B, sha256 `6e410974...dc5f85e0`, n=100,000,000,001,
runs=31,381,597,694, members m1,m4,m5,m7,m10,m11,m12,m13 (m1 codec 114, m13
codec 115: 27.7 GiB payload + 19.3 GiB of per-symbol run directory + 11.4 GiB of
chi this reader never touches).

Branch `fix/query-block-cache`, base `pile-week-v1` `d526d99`. Evidence root:
`/mnt/nvme2n1/erikg/blockcache-adjudication-20261106` (all journals, the scanner's
offset arrays, the grep transcript, the 192 missed contexts).

## 1. The regression: `--count` answered, `--limit` refused

The merged reader materialised the m1 run stream at open into three per-run
columns (heads u8 + bounded u32 lengths + u64 cumulative ranks). At this
container's 31,381,597,694 runs that is **408 GB** (414 GB with the row
directory), so `--count` worked and any locate refused at open instead of being
OOM-killed. The materialising arm is gone - not budgeted, not behind a flag:
the decoded-block cache is the only read path (port of `query-memory-bounded`
`2991df6`/`f00640d` onto this tree's u64 directories, count/limit modes and
document resolution).

Verbatim, load average 65.03 with another user's 24-thread and R jobs running:

```
xsa query /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 --limit 5 " function "
0       48898019802
0       1317846670
0       1317846350
0       68497132589
0       25206633999
QLITE_OPEN container=/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 format=sxi2-v6-lite
  magic=SXI2 version=6 mmap_ms=0.19 decoded_bytes=0 stages=none n=100000000001
  runs=31381597694 members=m1,m4,m5,m7,m10,m11,m12,m13
QUERY_LOCATE mode=limit positions_located=5 lf_steps=16398
QLITE_CACHE hits=22378 misses=13794 decodes=13794 drop_epochs=111
  arena_bytes=66846720 decoded_bytes=0 decode_ms=230.7 drop_ms=73.8
WALL 0.52 s RSS 560140 kB
```

`decoded_bytes=0` is not a label here, it is the audit line: the reader's only
per-run state is the fixed 64 MB arena (`arena_bytes=66846720`), and the peak RSS
is **547 MiB** for a 63 GB container.

```
xsa query <pile> --count "the "          -> 0  the          704942163   WALL 0.07 s RSS 210948 kB
xsa query <pile> --count "Harper Reed"   -> 0  Harper Reed          55  WALL 0.07 s RSS 213020 kB
xsa query <pile> --count " function "    -> 0   function       4785152  WALL 0.07 s RSS 215056 kB
```

All three unchanged, which is the point: the rewrite changed the memory model,
not an answer.

### What the bounded reader pays, measured (A/B against the base reader)

Same fixtures, one box, load average 47-78 during these runs (another user's
24-thread FastGA and R jobs are on the box all day; every wall below is labelled
with the load that was measured beside it). `BASE` is `d526d99` built in
`/tmp/bc-base-target`.

| | BASE 150 MB | HERE 150 MB | BASE 10 GB | HERE 10 GB | HERE pile |
| --- | ---: | ---: | ---: | ---: | ---: |
| open (`QLITE_BUILD`) | 105 ms | 15 ms | **61,092 ms** | 17 ms | 28 ms |
| run columns materialised | 754 MB | **0** | **45.8 GB** | **0** | **0** |
| query wall (whole gate) | 362 ms | 5,362 ms | 7.9 ms | 178 ms | 0.5 ms (`--count`) |
| end-to-end gate wall | 0.66 s | 5.4 s | 65.75 s | 0.23 s | 0.52 s (`--limit 5`) |
| peak RSS | 747 MiB | 110 MiB | **45.7 GiB** | 429 MiB | 547 MiB |

The trade is explicit and it goes both ways: at 150 MB the materialising reader
answers 15x sooner (it pays 105 ms once instead of ~2 us per block decode), and
at 10 GB it is already 285x SLOWER end to end (61 s of open against 17 ms) while
holding 45.7 GiB. The bounded reader's cost does not depend on n; the
materialising reader's does, and on this container it does not fit at all:

```
BASE on the pile, address space capped so it cannot allocate 414 GB:
  memory allocation of 125526390776 bytes failed      (the second run column alone)
  note: run with `RUST_BACKTRACE=1` ...  signal 6
```

i.e. `heads` (31.4 GB) + `lengths` (125.5 GB) + `seen` (251 GB) + the row
directory. The refusal the operator saw is the same arm with the memory check
tripping first (`qlite.rs:1046`, "this reader materialises the run stream at
open (414,210,770,038 bytes ...)").

Four measurements drove the final shape, each of them a real defect first:

1. **The payload is `pread`, not mapped.** The run stream is 27.7 GiB of the
   63 GB container and is read at random block positions; mapping it makes every
   decode a resident page and forces the RSS governor to sweep the mapping.
2. **`MADV_RANDOM` on the mapping.** Without it a 4 kB directory cell pulled in
   whole large folios (~370 kB of resident set per touch).
3. **`posix_fadvise(WILLNEED)`, not `madvise(WILLNEED)`.** The old prefetch
   faulted m1+m12+m13 into the process; at pile scale that is 20 GB of RSS for a
   five-position query. m1 is advised only over its header + block directory.
4. **The governor's drop is windowed (1 GiB, cursor cycling).** A whole-mapping
   `MADV_DONTNEED` over 63 GB measured **22 ms per call**, and a 20,000-position
   locate paid 933,925 of them (50,684 s of drop wall, 3,099 s of wall for
   20,000 positions). Windowed drops: 0.03-0.3 ms each.
5. **Per-lookup counters live in the cache shard**, under the lock the lookup
   already takes. As global atomics they were a single hammered cache line:
   `--limit 20000` took 334 s on 128 threads against ~880 s of serial work.

Locating is also parallel over the occurrence interval now (chunks of 1024 rows,
their own `Ctx`, results concatenated in row order - identical output), because a
serial 4.8 M-occurrence walk on this container is hours. Measured:
`--limit 20000 -j 128` = 149 s with the directory allowance, 573 s with the
512 MB budget the default uses.

## 2. The 192-offset discrepancy: grep undercounts, the index is right

`xsa query --count " function "` = **4,785,152**. `LC_ALL=C grep -a -o -F` =
**4,784,960**. Three other patterns agree exactly (`the ` 704,942,163,
`Erik Garrison` 10, `Harper Reed` 55).

A **third implementation** settles it: `tools/binbwt/pile_pos_scan.c`, a
windowed-mmap scanner that shares no code with either the index or grep, and
histograms the byte before and after every match. Over the same corpus
(`/mnt/nvme2n1/erikg/pile-head100.txt`, exactly 100,000,000,000 bytes):

```
SCAN pattern= function  bytes=100000000000 matches=4785152 windows=94
VERIFY pattern= function  n=4785152 ok=4785152 bad=0     # every offset re-read from the corpus
```

**The index is right and `grep -o` undercounts by exactly 192.** The mechanism is
`grep -o`'s non-overlapping-match rule meeting a pattern that can overlap itself:

- `" function "` is 10 bytes whose **first and last bytes are both a space**, so
  it has a 1-byte border and can occur again **9 bytes later**. `function
  function` contains two occurrences, one starting inside the other's last byte.
- `grep -o` reports leftmost, NON-OVERLAPPING matches, so after a match at `p` it
  resumes at `p+10` and never sees the match at `p+9`.
- `the `, `Erik Garrison`, `Harper Reed` have no border (`'t'`/`'E'`/`'H'` against
  a trailing space/`n`/`d`), so they cannot overlap themselves - which is exactly
  why those three agree and this one does not. The defect is narrow by
  construction, not by luck.

Evidence, all of it reproducible with `tools/binbwt/pile_search_certificate.sh`:

```
DIFF a=4785152 b=4784960 a_not_b=192      # scan \ grep   = the 192
DIFF a=4784960 b=4785152 a_not_b=0        # grep \ scan   = empty: no false positives
STAT count=4785152 unique=4785152         # the scan's set: no duplicate offsets
STAT count=4784960 unique=4784960         # grep's set
PAIRS missed=192 partner_at_minus9=192 partner_at_plus9=0 no_partner=0 scanner_has_both=192
```

Two independent confirmations that these 192 are real occurrences the *index
itself* knows:

- **Per-class decomposition.** xsa's counts for the 256 patterns `<byte> function `
  reproduce the scanner's preceding-byte histogram with **0 mismatches in 255
  comparable classes** (the 256th, `0x1E`, cannot travel in a record file and was
  counted separately: 2 on both sides), and 4,785,150 + 2 = 4,785,152.
- **The index locates them.** For each of the 192 offsets, a 42-byte window
  around it was extracted, and `xsa query <pile> --all -- <192 windows>` located
  234 positions in 6.1 s. Every window's start is present, and the 192 recovered
  offsets (+16 from the window start) are **identical as a set** to the 192 the
  scanner has and grep lacks:

```
VERIFY pattern= function  n=192 ok=192 bad=0
DIFF a=192 b=192 a_not_b=0     # xsa's locations vs the 192 scan-only offsets
```

Sample of the 192, with 55 bytes of context (`posset context`):

```
CTX offset=113113969 ... text="ct, but most applications assume the interface function function symbol() public pure returns (string memory), and when "
CTX offset=2939369380 ... text="mate); } }); } // Request animation frame loop function function animate(timestamp) { const delta = Math.min(timestamp -"
CTX offset=4976549049 ... text="mply written in terms of a single quaternionic function function $F(q_{NS},q_{NS'},q_{D4})$: $$F(q_{NS},q_{NS'},q_{D4}) "
```

Minimal reproduction (`grep (GNU grep) 3.11`), 43-byte file
`a function function b\n function  function \n`:

```
LC_ALL=C grep -a -b -o -F " function " ->  1, 22, 32        (3)
pile_pos_scan  (windowed mmap)         ->  1, 10, 22, 32    (4)
                                           ^^ the overlapping second " function "
```

Note the trap in the original report: "consecutive grep offsets never differ by 9,
so no overlapping matches exist". Offsets 9 apart is precisely what `grep -o`
**refuses to emit**; their absence is the symptom, not the disproof.

### Positions at scale

- `xsa query <pile> --limit 50000 -j 200 " function "`: 50,000 located positions
  in 1,048 s at load 65-77, then
  `posset verify corpus " function " <those 50k>` ->
  **50,000/50,000 carry the exact bytes, 0 bad**, 50,000 unique, and
  `posset diff <50k> <scan set>` -> **0 outside the scanner's set**.
- `xsa verify <pile> --corpus <corpus> --limit 200000 -j 200 " function "`
  (the largest run in this certificate): **200,000 located positions in 3,043.60 s
  at load 55-80, RSS 419924 kB, `VERIFY_TOTAL patterns=1 positions=200000 bad=0
  duplicates=0`**, 200,000 unique, and all 200,000 are members of the scanner's
  4,785,152-offset set (`posset diff` -> `a_not_b=0`). 4.2% of the answer, byte
  verified and set verified.
- `xsa verify <pile> --corpus <corpus> --limit 5000 -j 48 "the "`:
  `VERIFY id=0 len=4 occurrences=704942163 located=5000 unique=5000 bad=0 dup=0`,
  `VERIFY_TOTAL patterns=1 positions=5000 bad=0 duplicates=0`,
  WALL 206.33 s RSS 549356 kB.
- `xsa verify <pile> --corpus <corpus> --limit 5000 -j 64 " the "` (the
  spaced variant): `located=5000 unique=5000 bad=0`, WALL 223.96 s RSS 554736 kB.
- `Harper Reed` (55) and `Erik Garrison` (10): xsa's COMPLETE position sets
  (`--all`) are identical, offset for offset, to the independent scanner's.
- **The other direction - truth -> index.** 1000 offsets drawn uniformly from the
  scanner's 4,785,152-offset set (seed 20261106), a 42-byte window at each, and
  `xsa query --all -- <windows>`: 997 windows were representable as patterns (3
  contain the record separator or a NUL) and **997/997 window starts were
  located at exactly the offset they were taken from**
  (`positions_located=5743`, WALL 251 s, RSS 448 MiB). So the index knows every
  region of this file, not just the 192-offset neighbourhood.
- The full 4,785,152-position enumeration (`--all`) was **not** run end to end:
  the 200,000-position run above moved at 15.2 ms/position (9.5e8 LF steps,
  1.0e9 block decodes), so the whole answer is ~1.6e10 LF steps and 10-20 h at
  200 threads. Residual risk, stated plainly: what is proven is the count (total
  and 256/256 preceding-byte classes against an independent enumeration), the
  byte-exactness of 200,000 sampled rows with no duplicate, the reverse
  direction (997/997 random truth offsets located exactly), and the 192 rows grep
  dropped. What is *not* proven row by row is that no bogus row hides among the
  unsampled 96% - it would have to be a set of the same size, differing from the
  byte-level truth in fewer than 1 row in 24, and the class decomposition makes
  that implausible but not impossible.

## 3. `xsa verify`: the byte certificate as a verb

```
xsa verify <container> --corpus <corpus> [--limit N] [-j N] <pattern>...
```

It locates positions exactly like `query`, then reads the corpus back at every
offset and compares the bytes with the pattern, refusing a corpus whose size is
not `n-1` and failing loud on any mismatch or duplicate. Negative control: one
corpus byte flipped at a located offset gives

```
VERIFY_BAD id=0 offset=9292543 bytes="Zthe " pattern=" the "
VERIFY_TOTAL patterns=1 positions=50 bad=1 duplicates=0
xsa: error: verify: 1 located position(s) do not carry the pattern ...
```

## 4. Gates

- 150 MB `L150/t150m.sxi2` (sha256 `f6abd41d...f624b3e`, unchanged):
  `COUNTS 100/100 exact`, `POSITIONS 574/574 exact`, `GATE Q1 PASS`.
- 10 GB `g10/pile-10b-snap.sxi2` (sha256 `efd4628d...4b8609`, unchanged,
  n=10,000,017,259): `COUNTS 20/20 exact`, `POSITIONS 20/20 exact`,
  `GATE Q5 PASS`, `WALL 0.23 s RSS 439408 kB`.
- `cargo build --release`: no new warnings - **24** against the base `d526d99`'s
  25, the same set minus `cache_stats` (now used), nothing added;
  `cargo test --release`: 72 passed.
- Certificate script self-test on the 150 MB fixture with `" the "` (which also
  has a 1-byte border): `CERTIFICATE PASS`, 198 offsets grep missed there,
  200/200 index positions byte-verified.
