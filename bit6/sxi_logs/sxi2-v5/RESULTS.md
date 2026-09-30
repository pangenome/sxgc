# SXI2 v5 packed interval gate

The first gate was `python3 space_preflight.py`: it projected every requested
SXI2 file below its SXI1 source before any full conversion. Exact banked
Huffman, gamma, and EF chi bit counts were used. The achieved files match
every projected byte count. The writer used `--max-bytes` below the SXI1 file
length and published by a no-clobber link after full validation.

| Corpus | SXI1 bytes | SXI2 bytes | SXI2 / SXI1 | Runs | Chi count |
|---|---:|---:|---:|---:|---:|
| yeast235 | 1,804,487,132 | 1,413,689,216 | 0.7834 | 100,905,045 | 85,404,336 |
| pile-frag | 7,017,802,200 | 5,196,757,368 | 0.7405 | 397,723,010 | 306,164,765 |
| k10 | 33,961,712,446 | 28,372,620,184 | 0.8354 | 1,859,825,862 | 1,627,067,257 |

| Member, bytes | yeast235 | pile-frag | k10 |
|---|---:|---:|---:|
| 1 Huffman/gamma runs | 98,951,370 | 358,311,934 | 2,227,364,376 |
| 4 legacy anchors | 12 | 12 | 12 |
| 5 EF chi | 77,088,381 | 144,174,354 | 1,252,474,932 |
| 6 names / 7 remap | 351,876 | 256 | 0 |
| 8 packed phi intervals | 832,888,637 | 3,149,986,508 | 16,741,512,439 |
| 9 exact escape | 0 | 0 | 0 |
| 10 stored LF starts | 403,620,188 | 1,541,176,672 | 8,136,738,155 |
| 11 sparse SA values | 788,344 | 3,107,232 | 14,529,912 |

Directory, header, and alignment bytes account for the small difference
between member sums and exact file lengths. Members 2 and 3 do not exist in
v3. Member 11 stores one 8-byte SA value per 1024 runs. The v3 reader derives
run-tail samples and backward-search head toeholds from member 8.

## Validation

The writer validated each full partial container, decoded its complete EF chi
stream, and compared every value bytewise to the SXI1 source before publishing.
The exact chi counts above appear in the conversion logs. `test_sxi2.py`
passes repeated and periodic fixtures, including nonempty escape handling,
MEM and HTTP byte parity, and absence of members 2 and 3.

Native JSONL `xsa mems --min-len 50` on three source-derived 50-byte reads
passes byte parity for yeast (423 occurrence records), pile-frag (4), and
k10 (205). The three reads have occurrence counts 163/21/239, 1/1/2, and
185/10/10 respectively. The bounded `ropebwt3 -p 10` MEM output also passes
byte parity for yeast and pile-frag. `gate-summary.json` rechecks all
three native outputs, all three HTTP outputs, the exact source chi counts,
absence of raw sample members, and exact projection versus achieved bytes.

Warm HTTP `/query` with 16 sampled positions per strand and seed 7 passes
byte parity for yeast (32 lines, 5 requests), pile-frag (16 lines, 200
requests), and k10 (32 lines, 5 requests). Median latency was 5,076.7 ms
SXI1 versus 377.9 ms SXI2 for the yeast 50-mer, 0.353 ms versus 0.360 ms
for pile-frag's `and the`, and 10.23 ms versus 2.93 ms for the k10 50-mer. These
timings exclude index load, are single-client and single-worker, and include
HTTP handling plus locate. Pile-frag's earlier five-request probe is retained
separately but the 200-request result is the reported benchmark.

Cold CLI runs expose the load cost. Peak RSS was 1.70 GB old versus 2.51 GB
new for yeast, 6.68 GB old versus 9.74 GB new for pile-frag, and 31.10 GB
old versus 48.36 GB new for k10. The mapped
intervals are touched during validation; the stored LF member did not reduce
these measured peak RSS values. The final yeast reader took 55.91 seconds
versus 41.00 seconds for SXI1; k10 took 14:00.83 versus 3:23.40.
Exact times and RSS are in the `.time` files.

Writer wall times, including validation and chi comparison, were 1:37.19
(yeast), 7:01.32 (pile-frag), and 27:49.04 (k10). All source files remained
untouched. The temporary partial and bitstream files were removed after
publication.

## Remaining gap

The packed phi interval and LF members cost 98.0, 94.4, and 107.0 bits per
run at yeast, pile-frag, and k10. This exceeds the requested 5–12 bits per
run. Phi predecessor uses binary search, with `O(log R)` probes; it is not
`O(log log n)`. It takes at most 27, 29, and 31 interval probes per
successor at yeast, pile-frag, and k10 respectively; each packed EF select
advances across at most 63 one marks plus intervening zeros. The warm HTTP
measurements above are the observed query cost, including locate and HTTP
handling. The
466 projection using `n=1,403,221,068,481` and
`R=2,739,737,289` has a **42.83 GB lower bound** from phi, LF, and sparse
anchors alone, before runs and chi. The 6–8 GB stretch target fails for this
layout. Pile-frag is 4.802 times its text length, above the 0.6–0.7 stretch
target. Thus this is a full-scale, smaller-than-SXI1 experiment, but not the
requested few-bits-per-run Nishimoto–Tabei production structure.

The `contact_supervisor` intercom named in the task was absent from the tool
catalog, so the long gates ran in this worktree and their logs are retained
here. No source artifact was overwritten and no commit was made.
