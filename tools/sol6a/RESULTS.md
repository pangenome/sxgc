# sol6a: histogram-driven negative result

Decision: candidate (d), retain SXP4. The width budget fails before performance
engineering: on all 1,000,000,665 positions in the running chain's eight completed
chunk columns, 82.7881067% of within-4096-block deltas have magnitude >=32768.
Patched u16 therefore costs **5.311524270–6.967286405 B/pos**, even before
block bases, patch counts, alignment or indexing. The corresponding 100 MB
fraction is 75.1855254%, costing 5.007421015–6.511131523 B/pos.
A block-adaptive PFOR primary is almost always 28 bits at 1 GB, and optimistic
per-block exhaustive width selection costs about 3.5 B/pos. A split u16 hi/lo
column costs 4 B/pos. None achieves <=2.2 B/pos. SXP5 was not implemented;
this is the explicitly permitted clean negative pivot, not a speedup claim.

The measurements do not refute the decode-branching hypothesis. A wider SIMD
format could still improve wall time; these delta schemes cannot simultaneously
satisfy the stated scratch width. More sophisticated transforms or entropy coding
remain untested.

## Inputs and method

Base f81ceb8, branch sol-6a-aligned-pos. Read git log --oneline -10 first.
Scratch: /mnt/nvme2n1/erikg/sol6a-histogram/.

- 1 GB: /mnt/nvme2n1/erikg/sol6-compact-1b/chunks/chunk-{0..7}.pos,
  SXP4 v2, eight complete files, opened read-only. All positions decoded.
- 100 MB: /mnt/nvme2n1/erikg/extcols-scratch/mergperf-100m-sxp4c/chunks/*.pos,
  SXP4 v1, 16 files. The recent sol5i gate scratch has already deleted its columns.
  This older fixture uses smaller chunks than the current eight-chunk gate;
  that favors small delta widths, yet still fails the target.
- Independent reference: mergperf-100m-dense/chunks/*.pos, SXP3, same 16 chunks.
  Every histogram, exception count, block-width count and PFOR cost matched the
  decoded SXP4 v1 output exactly (diff after excluding the byte-size summary).

The standalone pos_histogram.cpp decodes canonical context Huffman bytes and
zigzag LEB128, reconstructing absolute positions and checking position bounds,
code lengths, input bounds and varint length. It uses a 16-bit Huffman lookup
with canonical slow fallback. All 4096-row group bases are excluded from delta
histograms, because they are stored absolutely. Subblocks of 512/1024/4096 rows
likewise exclude their first delta and use an 8-byte absolute base.
Widths are minimum **zigzag bit widths**, equivalent to signed primary widths
except at the negative power-of-two boundary. Exception fractions follow the
requested conservative magnitude >=2^(w-1) definition. Coverage thresholds use
ceil(percent * number_of_deltas / 100). Final partial blocks are included.

## Magnitude histogram

A bucket with width k is 2^(k-1) <= |delta| < 2^k. Zero counts are omitted.

| k | 100 MB count | 1 GB count |
|---:|---:|---:|
| 1 | 4,235 | 11,527 |
| 2 | 3,379 | 18,414 |
| 3 | 16,942 | 84,669 |
| 4 | 91,462 | 488,025 |
| 5 | 300,648 | 2,050,721 |
| 6 | 747,063 | 5,610,873 |
| 7 | 1,354,354 | 10,313,012 |
| 8 | 1,929,118 | 14,354,881 |
| 9 | 2,383,964 | 17,948,290 |
| 10 | 2,712,758 | 20,496,759 |
| 11 | 3,057,522 | 21,795,383 |
| 12 | 3,177,554 | 21,745,034 |
| 13 | 3,172,158 | 20,796,020 |
| 14 | 2,948,827 | 19,216,806 |
| 15 | 2,908,555 | 17,146,611 |
| 16 | 2,607,318 | 12,289,625 |
| 17 | 2,829,706 | 8,692,377 |
| 18 | 4,029,644 | 8,167,548 |
| 19 | 6,617,759 | 8,675,877 |
| 20 | 10,810,166 | 9,093,198 |
| 21 | 17,552,938 | 14,173,550 |
| 22 | 22,684,124 | 26,241,892 |
| 23 | 8,035,886 | 50,510,622 |
| 24 | 0 | 95,537,793 |
| 25 | 0 | 170,298,714 |
| 26 | 0 | 253,949,602 |
| 27 | 0 | 170,048,697 |

## Primary width and patch arithmetic

| Signed width | 100 MB exception fraction | 1 GB exception fraction | 1 GB B/pos, 4–6-byte patches |
|---:|---:|---:|---:|
| 12 | 87.395540% | 90.680476% | 5.127219019–6.940828529 |
| 16 | 75.185525% | 82.788107% | 5.311524270–6.967286405 |
| 20 | 59.097250% | 79.004643% | 5.660185714–7.240278571 |
| 24 | 0.000000% | 69.000281% | 5.760011232–7.140016847 |
| 27 | 0.000000% | 17.009011% | 4.055360442–4.395540663 |
| 28 | 0.000000% | 0.000000% | 3.500000000–3.500000000 |

The full exception table for widths 8 through 32 and full block distributions
are in histogram-100m.txt and histogram-1b.txt. The per-block exhaustive PFOR
estimate evaluates every width 0..32, adds an 8-byte base and 4 or 6 bytes per
exception, and picks the smallest result. It excludes headers, patch counts,
byte rounding, indexes and alignment, so actual storage would be larger.

## Block width distribution (1 GB)

Counts grouped by width, retaining all nonempty tails in the raw evidence.

| Block rows | Coverage | <=24 bits | 25 | 26 | 27 | 28 | Total blocks |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 512 | 100% | 2,085 | 118 | 361 | 1,986 | 1,948,578 | 1,953,128 |
| 512 | 99% | 3,384 | 330 | 922 | 5,669 | 1,942,823 | 1,953,128 |
| 512 | 90% | 13,534 | 2,775 | 10,905 | 155,279 | 1,770,635 | 1,953,128 |
| 1024 | 100% | 377 | 37 | 88 | 426 | 975,638 | 976,566 |
| 1024 | 99% | 769 | 108 | 283 | 1,522 | 973,884 | 976,566 |
| 1024 | 90% | 4,004 | 1,026 | 4,147 | 68,305 | 899,084 | 976,566 |
| 4096 | 100% | 18 | 4 | 9 | 28 | 244,086 | 244,145 |
| 4096 | 99% | 41 | 4 | 15 | 106 | 243,979 | 244,145 |
| 4096 | 90% | 320 | 118 | 543 | 13,003 | 230,161 | 244,145 |

## Actual existing storage and format locations

| Fixture | Rows | Actual .pos bytes | Actual B/pos | Existing format |
|---|---:|---:|---:|---|
| 100 MB | 100,000,503 | 328,663,481 | 3.286618278 | SXP4 v1 |
| 1 GB | 1,000,000,665 | 3,254,670,089 | 3.254667925 | SXP4 v2 |

These are existing artifact sizes, not sizes of a new codec. 100 MB SXP4 v2
storage was not regenerated. SXP3 reference is 800,004,664 bytes, 8.000006400 B/pos.
No production source changed. Existing specification/writer:
bit6/chunk_frontend.cpp:79 and :103; reader autodetection at
bit6/cross_lcp_merge.cpp:1156; group decoder at :1226. SXP3/SXP4 coexistence
is unchanged. There is no new SXP5 format specification.

## Gates, work, wall and scratch

No new merge/container gates or production selftests were run: the histogram
fails the width bar, and production source is byte-for-byte unchanged from base.
No claim is made of a newly observed work-counter identity check.
Historical baseline evidence in tools/sol5i/work-counters.json, verbatim:

- Gate A chi 29716349; cmp rc=0; comparisons=355643748,
  symbols_compared=4017075186, probes=90131924.
- Gate B chi 283296933; cmp rc=0; comparisons=3674629435,
  symbols_compared=60485597061, probes=1987510452.

Those results predate this task. No new wall comparison or du-sampled merge
scratch peak applies to this negative pivot. The running sol6 chain was not
signaled, rewritten, reconfigured or gated concurrently. Histogram elapsed
runtime is not a merge-wall measurement. RUNG10BV2_RC was absent on the checks;
no completion or dense A/B result is inferred. Existing .pos byte totals above
are exact, not sampled scratch peaks.

## Reproduction and limitations

Compile with c++ -O3 -std=c++17 tools/sol6a/pos_histogram.cpp -o
/mnt/nvme2n1/erikg/sol6a-histogram/hist, then supply the .pos paths listed above.
Compare 100 MB compact and dense reports after dropping their first summary
line. Full reports preserve integer counts rather than rounded percentages.

The intercom contact_supervisor tool was not present in the callable tool
catalog or PATH; no normal-text supervisor updates or questions were sent.
The result proceeds using the task's explicit histogram-negative authorization.

Residual risks: the 100 MB partition differs from the current eight-chunk gate;
PFOR estimates assume conventional full-value patches rather than a separately
compressed exception stream; no SIMD implementation or performance was measured;
future input distributions could differ. No production regression is introduced.
