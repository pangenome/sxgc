# SXI2 v6-2 explicit move association experiment

## Format and preflight

Format version 4 keeps the exact source-interval Elias–Fano boundaries and
their paired destination and original BWT run ID fields from v5 member 8.
It removes the stored LF-start payload: member 10 has zero bytes, and the
reader reconstructs each LF destination from the Huffman/gamma run stream,
the C table, and the cumulative length for that run's character. Member 11
sparse SA anchors and the v3 exact escape semantics are unchanged. The new
reader also accepts v3 artifacts.

The space preflight ran before conversion. It projects 1,010,069,024 bytes
(yeast235), 3,655,580,696 bytes (pile-frag), and 20,235,882,024 bytes (k10),
all below the v5 gate references. Exact member calculations are in
`space-preflight.json`.
The `sxi2-v6/RESULTS.md` and `space_preflight.py` named in the task were
absent from this checkout and from `main`; the supplied optimistic figures
were used as the comparison reference.

The requested 25–30 bits/run target is not met by this generic exact form:
phi plus LF are projected at 66.033, 63.360, and 72.013 bits/run. The
25.146/27.124/29.350 bits/run in the task equal `log2(R!)/R`, the entropy of
**one** unconstrained run permutation. A representation with independently
sorted source and destination boundary sets needs two run associations to
support both source predecessor lookup and original-run SA sample lookup.
The generic two-permutation, two-boundary-set floors are 63.228, 59.412,
and 69.532 bits/run (see `association-floor.json`). BWT-specific correlation
could reduce this; deriving it was explicitly deferred in the task. These
are generic layout bounds, not an impossibility theorem for BWT data.

## Verification

The writer and reader were freshly built from this worktree; their SHA256
hashes are retained in `binary-sha256.txt`. `test_sxi2.py` passed repeated and
periodic fixtures, exact chi, MEM and HTTP byte parity, and a direct header
assertion that the built writer emits format version 4. Full corpus gates are
recorded below when complete.

Yeast235 has published at the exact projected 1,010,069,024 bytes, with
100,905,045 runs and 85,404,336 exact chi values. The writer's full partial
validation and bytewise SXI1 chi comparison passed. Full native MEM output
and bounded MEM output match SXI1 byte-for-byte; HTTP `/query` output also
matches byte-for-byte. Five warm locate-heavy HTTP requests had median
382.3 ms versus the v5 gate's 377.9 ms. This is within run-to-run noise,
not evidence of a throughput improvement.

Pile-frag has published at the exact projected 3,655,580,696 bytes, with
397,723,010 runs and the remapped chi count 306,164,765. Writer validation
and bytewise chi comparison passed. Native MEM, bounded MEM, and 200-request
HTTP outputs match SXI1 byte-for-byte. Warm HTTP median was 0.362 ms,
compared with the v5 200-request reference of 0.360 ms. This is effectively
unchanged throughput at the tested query.

K10 has published at the exact projected 20,235,882,024 bytes, with
1,859,825,862 runs and 1,627,067,257 chi values. Full partial validation
and bytewise chi comparison passed. Its native MEM output matches SXI1
byte-for-byte (205 records) after a 862.7-second cold run. The first HTTP
attempt captured the SXI1 response but timed out after 900 seconds waiting
for the v6-2 server to finish loading. `k10-http.log` retains this failed
attempt. A second attempt uses a 2400-second startup deadline and compares
against the captured SXI1 response (SHA256
`bc429f50693789eae9acbbf877dc9da5a61d2c8d8933fdf181fd6d0076bcdb6d`).
The retry passed: 32 response records, exact byte parity, and 3.474 ms warm
median versus v5's retained 2.926 ms median for the same 50-mer and sampling.

## Achieved space

All three published files match the exact preflight member sizes and are
smaller than v5. Byte counts below are decimal.

| Corpus | v5 bytes | v6-2 bytes | v6-2 / v5 | Phi + LF bits/run | Supplied optimistic floor |
|---|---:|---:|---:|---:|---:|
| yeast235 | 1,413,689,216 | 1,010,069,024 | 0.7145 | 66.033 | ~575.9 MB |
| pile-frag | 5,196,757,368 | 3,655,580,696 | 0.7034 | 63.360 | ~1.982 GB |
| k10 | 28,372,620,184 | 20,235,882,024 | 0.7132 | 72.013 | ~11.58 GB |

| Member bytes | yeast235 | pile-frag | k10 |
|---|---:|---:|---:|
| 1 Huffman/gamma runs | 98,951,370 | 358,311,934 | 2,227,364,376 |
| 4 legacy anchors | 12 | 12 | 12 |
| 5 EF chi | 77,088,381 | 144,174,354 | 1,252,474,932 |
| 6 names / 7 remap | 351,876 | 256 | 0 |
| 8 source EF + destination/run association | 832,888,637 | 3,149,986,508 | 16,741,512,439 |
| 9 exact escape | 0 | 0 | 0 |
| 10 derived LF starts | 0 | 0 | 0 |
| 11 sparse SA anchors | 788,344 | 3,107,232 | 14,529,912 |

| Member bits/run | yeast235 | pile-frag | k10 |
|---|---:|---:|---:|
| 1 runs | 7.845 | 7.207 | 9.581 |
| 5 chi | 6.112 | 2.900 | 5.387 |
| 6 names / 7 remap | 0.028 | <0.001 | 0 |
| 8 phi source/destination association | 66.033 | 63.360 | 72.013 |
| 10 derived LF | 0 | 0 | 0 |
| 11 sparse anchors | 0.063 | 0.063 | 0.063 |

## Query and verdict

`gate-summary.json` independently rechecks the published version, exact
projection, below-v5 sizes, chi counts, absence of raw samples, native MEM
byte parity, bounded MEM byte parity where applicable, and HTTP response
hashes. All fields pass. Native MEM record counts are 423, 4, and 205.

| Warm HTTP median | v5 reference | v6-2 | Result |
|---|---:|---:|---|
| yeast235 50-mer | 377.9 ms | 382.3 ms | same within run variation |
| pile-frag `and the`, 200 requests | 0.360 ms | 0.362 ms | same within run variation |
| k10 50-mer | 2.926 ms | 3.474 ms | about 19% slower in these five requests |

The gate uses single-worker warm HTTP responses and 16 sampled positions per
strand. The v5 timings are from the retained prior gate, so the small timing
differences are descriptive rather than controlled paired estimates. The
v6-2 k10 cold native run took 14:22.7 versus the retained v5 14:00.8.
Observed writer RSS was about 44 GB and reader RSS remained far below the
900 GB cap.

**Verdict:** exact byte parity, exact chi, version-by-binary, and space below
v5 pass at all three scales. The requested ~25–30 bits/run and ~0.6/2.0/11.6
GB sizes do **not** pass: the achieved files are ~1.01/3.66/20.24 GB.
Reducing the remaining generic association cost requires a BWT-specific
derivation or different query contract; that research was deferred in the
task. No retained v5 artifact or protected 466/BCR gate was overwritten.
The `contact_supervisor` intercom requested for long-gate handoff was absent
from the active tool catalog, so the gates were run to completion here.
