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

## Optimistic block-adaptive PFOR storage

| Rows/block | 100 MB B/pos, 4-byte patches | 100 MB, 6-byte patches | 1 GB, 4-byte patches | 1 GB, 6-byte patches |
|---:|---:|---:|---:|---:|
| 512 | 2.990297239 | 2.994858372 | 3.491279582 | 3.499889065 |
| 1024 | 2.990259306 | 2.993555494 | 3.492564559 | 3.499098505 |
| 4096 | 2.991373481 | 2.992952097 | 3.496150762 | 3.499415684 |

These are conventional full-value patch estimates with unrealistically cheap
metadata, not a lower bound for all possible separately compressed exceptions.

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
No new-format work-counter identity check applies; the completed compact leg
was read back and matched the base counters exactly (details below).
Historical baseline evidence in tools/sol5i/work-counters.json, verbatim:

- Gate A chi 29716349; cmp rc=0; comparisons=355643748,
  symbols_compared=4017075186, probes=90131924.
- Gate B chi 283296933; cmp rc=0; comparisons=3674629435,
  symbols_compared=60485597061, probes=1987510452.

Those results predate this task. During analysis the shared chain completed
its compact leg and started dense chunking. Read-only checks now additionally
establish for that compact leg:

- PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609.
- cmp against extcols-scratch/pilev2-g2-1b/frag.sxi2 returned 0.
- comparisons=3674629435 symbols_compared=60485597061 probes=1987510452:
  all three identical to the recorded post-sol5i base.
- merge-v2.time: elapsed 22:26.65 (1346.65 s), exit status 0;
  CROSS_KWAY wall_total=1346.582.
- sol6.log: PEAK_DISC_USED_BYTES 6355855237, du-sampled every 2 seconds
  across concurrent merge+finish. Exact previously measured .pos bytes:
  3254670089.

**Timing label: contaminated ceiling** (the finish and these read-only histogram
scans overlapped the merge). This is the shared chain's existing compact leg,
not a new-format trial or a newly launched sol6a merge. Dense wall remains pending;
there is no candidate wall or candidate scratch peak because no codec was landed. The running sol6 chain was not
signaled, rewritten, reconfigured or gated concurrently. Histogram elapsed
runtime is not a merge-wall measurement. RUNG10BV2_RC was absent on the checks;
the compact-leg completion was subsequently visible in phase logs, while no
completed dense A/B result is inferred. Existing .pos byte totals above are exact.

## Analyzer correctness checks

check-v2.py creates 8192 shuffled positions in both SXP3 and an independently
encoded SXP4 v2, using variable canonical Huffman lengths and two 4096-row groups.
The analyzer's lookup path, its --slow bitwise path, and the dense reference
produce identical histograms, exception fractions, PFOR costs and coverage
widths (exclude only the size summary when comparing dense).
The all-100-MB real SXP4 v1 versus dense identity check also passed.

An attempted additional full real chunk-0 lookup/slow recheck returned assertion
failure at opening the input: the shared chain had just legitimately deleted
consumed chunk files. Both complete real 1 GB histogram passes had already
finished successfully and agreed on all preexisting fields; this disappearance
does not invalidate their evidence. No chain process was interrupted.

## Reproduction and limitations

Run python3 tools/sol6a/check-v2.py to reproduce the synthetic inputs.
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
