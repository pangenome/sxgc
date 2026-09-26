# SLIM measurements and conditional 466 projection

All GB below are decimal. These are **component measurements, not a passed
G2 gate**: G1 is blocked by the supplied yeast ri4's missing SA samples.
The construction headline is not established. Logs are in
`bit6/gate_logs/slim/`; larger binary artifacts remain in `/tmp/laneQ/`.

## What was measured

Yeast: n=3,336,986,760; r=100,904,881; P=34,134,006;
virtual dictionary D=513,395,745 bytes, 3,159,456 distinct phrases.
The virtual dictionary adds nine dollar bytes, matching the vendored parser.

`--slim-profile-build --dict-stream` builds every slim LCE and LF structure,
but deliberately neither resolves positions nor writes an aggregate. It
accepts placeholder samples only for this explicitly named diagnostic mode.
Its latest run took **21.56 seconds, 3,228,076 KiB peak RSS (3.30555 GB)**.
Phase peaks are cumulative process high-water RSS, not isolated allocations.

| Phase | Wall seconds | Cumulative peak RSS, KiB |
|---|---:|---:|
| ri4 load + dictionary open | about 2.3, inferred remainder | 1,677,312 |
| dictionary sparse boundaries (two scans) | 5.033 | 1,679,360 |
| parse read + sparse text boundaries | 9.101 | 1,855,488 |
| parse suffix fingerprints | 0.183 | 1,945,600 |
| dictionary suffix fingerprints | 2.969 | 2,041,856 |
| LF tables | 2.052 | 3,228,076 |
| full resolve + LCE + streamed aggregate | **not run: missing samples** | unmeasured |

All phases are structured to avoid dense n-bit or D-bit boundaries: builders
produce sparse SDSL sd_vectors directly, and transfer their storage into the
finished vectors. No suffix arrays, inverse arrays, LCP arrays, RMQs, M,
b_bwt, or w_wt are created on the slim path, including transiently.

| Retained structure | Yeast bytes |
|---|---:|
| fixed-width uint32 parse, including zero terminator | 136,536,028 |
| parse fingerprints, tau1=3 | 91,024,024 |
| dictionary text, resident alternative | 513,395,736 |
| dictionary text, streamed mode | 0 resident text bytes; 262,144 cache bytes/thread |
| dictionary fingerprints, tau2=41 | 100,174,784 |
| sparse b_p, including select supports | 41,215,978 |
| sparse b_d, including select supports | 3,910,008 |
| ri4 characters + lengths | 504,524,405 |
| ri4 row starts | 807,239,048 |
| packed ri4 tail samples | 403,619,528 |
| LF per-character run IDs + prefix sums, actual capacities | 1,344,159,228 |
| aggregate chunk: 65,536 rows × four uint64 fields | 2,097,152 |

The dictionary cache uses four fully associative 64 KiB pages per thread,
read with pread. No mmap mapping can quietly make the whole dictionary
resident. Kernel filesystem cache is not process RSS and is reclaimable;
its I/O/throughput consequences are still real. Normal `--slim` defaults to
resident dictionary for small inputs; **use `--dict-stream` for the memory
projection below**. Stream mode is gated on every battery text, and its
yeast primitive check below exercises actual dictionary reads.

## Query-time sensitivity (partial G1 diagnostic)

`tools/slim_lce_probe.cpp` checks topLCP and interiorMin on 100,005 spread
runs against the baseline. It **takes SA endpoints from the baseline** and
therefore does NOT validate the LF resolver or establish end-to-end
construction. All four configurations have zero LCE mismatches. They each
perform 35,978 parse LCE and 191,466 dictionary LCE calls, verifying
2,684,569 phrase IDs and 6,416,779 dictionary bytes. Reported query wall
includes baseline seeks and reads and is single-threaded.

| Sampling | tau1 | tau2 | Fingerprints MB | Query seconds | Whole diagnostic peak KiB |
|---|---:|---:|---:|---:|---:|
| ceil(P/r), ceil(D/r), resident D | 1 | 6 | 957.600 | 2.096 | 1,617,920 |
| default ceil(8P/r), ceil(8D/r), resident D | 3 | 41 | 191.199 | 2.704 | 868,352 |
| default, streamed D | 3 | 41 | 191.199 | 8.021 | 364,544 |
| ceil(32P/r), ceil(32D/r), resident D | 11 | 163 | 50.022 | 2.517 | 729,088 |

These short, warm-cache runs do not establish a human query-time bound.
No extrapolation turns their baseline-provided positions into free position
resolution: the unchanged v4 LF walk's previously measured k10 cost remains.

**Exactness versus query complexity:** sampled polynomial hashes propose a
prefix using exponential/binary search. Every proposed matched symbol and
the first excluded symbol are then read directly. A collision yields a
nonzero fatal exit rather than a wrong accepted answer. This gives verified
answers in O(tau log L + L) time, not the requested O(tau) worst case.
Satellite mega-runs still pay the full direct verification scan. A final
partial-block check alone cannot prove that earlier hash-equal blocks match.
The executable reports verified-symbol counts and hash probes for this reason.

## Sweep measurements on the baseline yeast aggregate

Both modes produced byte-identical 683,233,920-byte witness files. The
streamed output also passed NumPy sorted-set equality against
`chi_yeast_pfp2.sA`, with chi=85,404,240. This validates the sweep separately,
not the unperformed slim aggregate construction.

| Sweep | Wall seconds | Peak RSS KiB |
|---|---:|---:|
| existing resident aggregate/index/output mode | 107.90 | 6,023,932 |
| `--stream-agg`, buffered aggregate, characters and witness | 15.36 | 4,096 |

The streamed reader retains four 512 KiB aggregate buffers, a 1 MiB run-char
buffer and a 1 MiB output buffer. It never loads lengths/samples/LF tables or
retains the witness list. CRA1 and raw little-endian uint64 witness formats
are unchanged. Baseline mode still exists for byte comparisons.

## 466 arithmetic

Assumptions: n466=1.403e12, r466=2.74e9, as in the committed handoff.
The actual on-disk k10 inputs were inspected: P=333,531,723 at
n10=30,151,407,545; dictionary disk size=5,483,754,397 bytes,
virtual size=5,483,754,406; 37,705,212 distinct phrases (stream-counted
EndOfWord bytes). This measured file differs from the 5,446,049,184-byte
figure in the handoff's introductory table; the handoff's retained dict
structure size already corresponds to approximately 5.48 GB.

Use the human P/n, rather than an assumption that n scales with run count:
P466=15.51984 billion, versus 14.35127 billion using yeast's P/n (8.1% lower).
D466=255.169 GB at constant k10 D/n; distinct phrases=1.75449 billion at
constant k10 dictionary phrase density. Their true growth is unknown. The
projected phrase ID alphabet fits uint32 under these assumptions.

Sparse boundary bytes use measured yeast bytes/phrase, scaled to projected
P and distinct dictionary phrase count. The P/n and D/phrase ratios remain
in the same Elias-Fano low-bit-width classes at k10 as yeast. Select overhead
and allocator behavior at 466 remain unmeasured.

Default tau1=ceil(8P/r)=46 and tau2=ceil(8D/r)=746. The factor eight is a
constant in the requested P/r and D/r classes and is **essential** to this
projection. It is not the literal tau=P/r memory budget.

| Query/build component | 466 GB |
|---|---:|
| uint32 parse | 62.079 |
| sparse b_p | 18.740 |
| sparse b_d | 2.171 |
| parse fingerprints | 2.699 |
| dictionary fingerprints | 2.736 |
| ri4 characters + lengths | 13.700 |
| ri4 starts | 21.920 |
| packed ri4 tail samples, 41 bits/sample | 14.043 |
| LF tables, vector-capacity upper bound | 43.840 |
| chunks and 64 dictionary caches | 0.019 |
| subtotal | **181.947** |
| explicit unmeasured allocator/support reserve | **10.000** |
| conditional projected peak | **191.947** |

LF bound: char-run vectors retain <2r uint32 capacity, or <8r bytes;
prefix sums use 8r bytes. During char-run vector growth, no prefix sums have
yet been allocated; the old/new allocation overlap is bounded below the
16r final bound. The legacy LF implementation is unchanged. Boundary/hash
construction precedes LF construction, avoiding overlap of LF tables with
boundary-builder temporaries. Each run's ID still fits uint32 at r466.

The largest terms are the parse (62 GB) and existing ri4/LF machinery
(93.5 GB). Only **8.05 GB of margin remains below 200 GB**, after the stated
10 GB reserve. This is a conditional model, not an observed 466 guarantee.
At fixed R, P growth beyond this estimate or larger boundary overhead can
exceed the target. Resident dictionary text raises the model to **447.116
GB**. Literal ceil(P/r), ceil(D/r) sampling raises streamed peak to **228.921
GB**. Halving D/n mostly reduces disk and scan costs in streamed mode; it
does not remove the parse and resolver terms.

At 466 the CRA1 sidecar is 87.68 GB on disk; the required raw witness file
is about 18 GB. The streamed sweep retains approximately 4 MiB of buffers,
independent of their sizes. The parser creating P and D, filesystem cache,
and artifact staging disk space are outside these process-RSS figures.
No 466 build was attempted. All measured process peaks are below 150 GB.

## Pass-2 resolver audit — no new cost claim

The proposed replacement for LF is blocked by its actual dependencies:
`pfp_sa_support` reads M, b_bwt, w_wt and saP (sa_support.hpp:55-66).
The Python parse resolver has equivalent dependencies. Reusing either
would violate the pass-2 no-M/b_bwt/w_wt constraint. Consequently the
191.947 GB pass-1 projection above remains conditional on the LF/sample
route; it is **not** a projection for a demonstrated parse-resolve build.
Subtracting LF/sample bytes without adding a measured replacement would
understate memory. No parse-resolve wall time, RSS, or 466 projection has
been measured or established.

Existing primitive logs yield 2,684,569 / 35,978 = 74.61696 verified phrase
positions per counted parse fingerprint query, including an unequal boundary
when present. This is an arithmetic summary of the pass-1 oracle-seeded
100,005-run probe, not an average over a completed yeast construction.
The query maximum was not instrumented in those logs and cannot be inferred
from the total; the requested full seed-query mean and maximum remain
unmeasured. No final G2 acceptance is claimed. See the pass-2 section of
SLIM_HANDOFF.md and pass2-resolver-audit.log for the blocking source evidence.
