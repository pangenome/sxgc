# SXI2 v6 preflight: requested move form does not meet its space gate

Run `python3 bit6/sxi_logs/sxi2-v6/space_preflight.py` from the repository
root. It exits 1 and writes a JSON report to stdout. The retained
`space-preflight.json` is that output. No full conversion was started.

## Structural finding

The proposed two sorted integer sequences do not encode an interval move.
The destination start values are sorted only in **destination** order; the
query needs the association with **source** order. Sorting both lists drops
that association. In [Nishimoto and Tabei, Section 3.2](https://arxiv.org/html/2006.05104v3),
the move structure stores pairs `(p_i,q_i)` plus a destination interval index:
three machine words per interval after balancing, with at most twice as many
intervals. The paper measures space in *words*, not 5–12 bits per run. Its
example has source starts `(1,2,3,7,14)` and destinations
`(10,11,12,1,8)` in corresponding order. The destination sequence is not
monotone. [Movi 2](https://academic.oup.com/bioinformatics/article/42/7/btag362/8710943)
likewise lists the move row ID as a field requiring `log2(r)` bits before its
compression techniques.

For the requested independent interval lists, even an ideal encoding of the
source-to-destination association has `r!` possibilities, or `log2(r!)` bits.
The sorted source boundaries need `log2(binomial(n,r))` bits if independently
stored. The following accounting retains actual v5 member lengths for runs,
EF chi, names/remap, and anchors; it **omits** the second sorted list, select
directories, checksums, and alignment. It is therefore an optimistic budget
for the requested representation, not an achieved v6 size. It is not a lower
bound on a different algorithm that derives the association from the BWT
while querying.

| Corpus | `r` | Pairing bits/run | Source boundary bits/run | Retained members | Optimistic total | v5 achieved | SXI1 |
|---|---:|---:|---:|---:|---:|---:|---:|
| yeast235 | 100,905,045 | 25.146 | 6.468 | 177,179,983 B | 575,929,770 B | 1,413,689,216 B | 1,804,487,132 B |
| pile-frag | 397,723,010 | 27.124 | 2.581 | 505,593,788 B | 1,982,433,735 B | 5,196,757,368 B | 7,017,802,200 B |
| k10 | 1,859,825,862 | 29.350 | 5.416 | 3,494,369,232 B | 11,576,723,128 B | 28,372,620,184 B | 33,961,712,446 B |

The association term alone exceeds 12 bits/run at every scale. The fragment
optimistic total is 1.98 GB, already above the requested 0.9–1.3 GB.
For the 466 assumptions (`n=1,403,221,068,481`, `r=2,739,737,289`),
association plus source boundaries alone are about 13.82 GB before runs,
chi, or anchors. At the token-space assumptions (`n≈264 million`,
`r=192,097,724`), the same terms are about 654 MB before runs, chi, or the
byte-offset table. Neither is a v6 artifact projection.

## Per-member baseline and gates

| Retained v5 member, bytes | yeast235 | pile-frag | k10 |
|---|---:|---:|---:|
| 1 entropy-coded runs | 98,951,370 | 358,311,934 | 2,227,364,376 |
| 4 old anchors | 12 | 12 | 12 |
| 5 EF chi | 77,088,381 | 144,174,354 | 1,252,474,932 |
| 6 names / 7 remap | 351,876 | 256 | 0 |
| 11 sparse SA | 788,344 | 3,107,232 | 14,529,912 |

| Gate | Result |
|---|---|
| v5 < SXI1 at all three scales | PASS, banked sizes |
| v6 < v5 at all three scales | NOT RUN; no valid v6 codec or projection |
| v6 move members 5–12 bits/run | FAIL for the requested independent-list form |
| fragment 0.9–1.3 GB | FAIL even under optimistic independent-list budget |
| Full artifacts, MEM parity, exact chi, HTTP, differential | NOT RUN; preflight stopped conversion |
| v6 locate-heavy throughput | NOT RUN; no v6 reader |

The banked v5 reference reported exact chi counts of 85,404,336,
306,164,765, and 1,627,067,257 respectively. No v6 chi stream exists to
compare. Its single-worker warm HTTP locate measurements are retained here
only as a baseline; the v6 column is unmeasured.

| Corpus and query | SXI1 median | v5 median | v6 median |
|---|---:|---:|---:|
| yeast235 50-mer | 5,076.7 ms | 377.9 ms | not run |
| pile-frag `and the` | 0.353 ms | 0.360 ms | not run |
| k10 50-mer | 10.23 ms | 2.93 ms | not run |

The source tree also does not contain the shipped v5 writer/reader: the
current `bit6/sxi2_write.cpp` emits version 2, codec 108 and members 2/3,
while the banked v5 artifacts are version 3, use codec 118, omit members 2/3,
and add members 10/11.
The v5 implementation exists staged in the earlier v5 agent worktree, but
`git show 20901f0 --name-only` shows it was not banked by the v5 commit.
The stated `bit6/sxi_logs/token-space/` directory is likewise absent from
this checkout; its measurements are recorded in `RESEARCH.md`.

No source artifact was changed or overwritten. No full-scale gate was
launched. The task's `contact_supervisor` intercom is absent from the exposed
tool catalog, so no decision request could be sent through it.
