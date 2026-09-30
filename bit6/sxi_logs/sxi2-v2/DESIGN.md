# SXI2 compact published form: v2 design review

**Status: blocked before publication.** The required predecessor review files
`bit6/sxi_logs/sxi2-review/{DESIGN,RESULTS}.md` are absent from this worktree
and every sibling worktree inspected. This note reconstructs the design from
the retained SXI1 artifacts and the two cited papers. It is a candidate wire
layout, not a declaration that a compatible writer or reader exists.

## Scope and source of truth

The publish operation consumes the already certified `.ri4`, `head_sa`, `.sA`,
XANC, names and remap artifacts. It never reconstructs text, changes parsing,
repairs seams, or walks `n` BWT rows. The build-time head and mirrored-tail
samples may be transformed into a locate map, but SXI2 must not contain the
SXI1 raw head and tail members. Publication uses a new output path, a partial
file, complete validation, fsync, and atomic no-clobber link. SXI1 remains
the canonical format and its byte contract is unchanged.

The [Nishimoto--Tabei paper](https://arxiv.org/pdf/2006.05104), Lemmas 3--4,
Theorems 7--9, supplies **two distinct** interval permutations: the LF map
on BWT rows and the `phi^-1` map on SA values. The [Movi 2 repository](https://github.com/mohsenzakeri/Movi)
is the locality and compressed LF-move reference; its LF row table alone
cannot locate every SA row. In particular, `SA[tail] != SA[head] + run_length`
in general.

## Candidate container

Keep the 64-byte header and 40-byte ordered directory shape of SXI1, but use
magic `SXI2`, version 2 and codec IDs independent of member IDs. The mandatory
members are:

| ID | Member | Source / required operations |
|---|---|---|
| 1 | Entropy-coded RLBWT | Canonical Huffman run heads, Elias gamma positive lengths, 256-symbol `C`, independently seekable checkpoints. Decode run, rank and select without expanding text. |
| 2 | LF interval map | For BWT run `i`, input start `p_i`, output start `q_i=C[c_i]+rank(c_i,p_i)`; output interval has the same run length. Store a compressed move table and output-to-input interval hint or a predecessor structure. |
| 3 | SA-value interval map | Sort the **actual** tail SA values `u_i=n-1-mirrored_tail_i`. Store each `u_i` and `v_i=SA[(tail_row_i+1) mod n]`, which is the next run's head SA, plus the interval navigation hints. This is `phi^-1`, not LF. |
| 4 | Sparse explicit anchors | Initial/toehold SA value, optional exceptional cyclic or separator anchors. The count and semantics must be explicit; an undocumented sampling stride is invalid. |
| 5 | Chi set | Elias--Fano over sorted unique positions in `[0,n]`, including the virtual end. Retain monotone streaming decode for `sxi-info --chi-out` and future `xsa forks`. |
| 6 | Names, optional | Same verbatim TSV semantics as SXI1. |
| 7 | Byte permutation, optional | Same 256-byte permutation and 0x1E fixed point as SXI1. |

All mandatory members need count, byte length and CRC in the directory;
readers must verify exact section bounds and checksums before mapping them.
The coded RLBWT needs bit lengths and checkpoint offsets. The LF and SA-value
maps need a specified codec, predecessor/index guarantees, and independent
semantic validation; opaque byte blobs cannot satisfy this design. Avoid
promising a 3-bit-per-run phi map until its actual encoding has been measured.

## Recovery proof obligations

For any BWT row `j` in run `i`, `LF(j)=q_i+(j-p_i)`. Thus one LF move uses
the run interval and a predecessor or stored destination interval hint.
Backward search extends both boundaries using these moves and the run-head
rank/select support. The output interval alone gives the count; it does not
give the SA values.

For an aperiodic cyclic text with a sound BWT order, the SA-value map uses
`u_i=SA[tail_i]` and `v_i=SA[tail_i+1]`. For `x` in the domain interval
starting at `u_i`, `phi^-1(x)=v_i+(x-u_i)` modulo `n`; applying it to a known
`SA[b]` yields `SA[b+1]`, then every occurrence in SA order. The toehold
must be carried by modified backward search: if the selected BWT position is
the current interval head, keep its SA; if it is a later run head, use its
stored head SA (`v` in member 3); then one LF move decrements the SA
coordinate. This is the paper's `SA+` and `SA+_index` machinery. An arbitrary
row cannot be located with one phi move unless a toehold is already known.
The current `xsa mems` matching-statistics path asks for arbitrary rows, so a
real port must propagate toeholds through those interval states and enumerate
with `phi^-1`. A fallback walk per occurrence does not meet the query design.

The paper assumes a unique terminal symbol. Our SXI1 indexes one **cyclic**
string with repeated separators; periodic strings can have equal rotations
whose order is fixed by position. The paper's interval slope does not
automatically survive these ties. The direct oracle `T=(ACG 0x1E)^4` has
`n=16`, only four BWT runs, and the tail-derived phi map gives `phi^-1(0)=1`
while the true SA successor is `4`. `T=(0x01 0x02 0x02)^2` requires another
exception at SA value 3. A correct SXI2 writer must either encode a proven
O(R)-space periodic exception map and validate it from the retained inputs in
O(R polylog R), or reject periodic corpora before publishing. Neither path is
implemented. A small fixture passed by chance would not prove correctness.

Chi correctness is independent of query structures. Decode the Elias--Fano
low bits and high unary vector in increasing rank order to get exactly the
sorted source `.sA` values. Check `0 <= x <= n`, strict increase, cardinality,
and no trailing bits. Re-encode/decode and compare a streaming SHA256 or
bytewise u64 stream against the sorted source before link publication; merely
matching chi counts is insufficient. The carried gates are yeast235
85,404,336; k10 1,627,067,257; **remapped** pile-frag 306,164,765.

## Space and latency budget

`size_probe.py` measures source-symbol Huffman and gamma bit counts exactly,
and calculates the plain Elias--Fano bit-vector length; it is **not** an SXI2
writer. See `size-probe.json`. Decimal GB are used below.

| Corpus | n | R | chi | SXI1 GB | Ideal Huffman+gamma GB | Ideal EF GB | Before maps GB |
|---|---:|---:|---:|---:|---:|---:|---:|
| yeast235 | 3,336,986,759 | 100,905,045 | 85,404,336 | 1.804 | 0.099 | 0.077 | 0.176 |
| pile-frag remapped | 1,082,130,213 | 397,723,010 | 306,164,765 | 7.018 | 0.358 | 0.144 | 0.502 |
| k10 | 30,151,407,545 | 1,859,825,862 | 1,627,067,257 | 33.962 | 2.227 | 1.252 | 3.479 |

The brief's `~0.5 GB` EF estimate for pile-frag is an arithmetic unit error:
`chi*(2+log2(n/chi))/8` is about **0.146 GB**, and the exact simple EF
layout measured here is **0.144 GB**. The older ~0.552 GB pre-locate
estimate leaves ~0.050 GB above the ideal source and chi codecs for LF and
codec/checkpoint overhead. A 0.15--0.25 GB phi member would give ~0.702--0.802
GB before additional overhead, or ~0.65--0.74 times the 1.082 GB text. The
upper part exceeds the requested 0.6--0.7 target. The phi member's actual
encoded bytes, rather than an assumed few bits per run, decide the verdict.

The 466 projection cannot be established by scaling a fragment with a very
different `R/n`. The current 466 ledger gives n=1,403,221,068,491,
R=2,739,737,289, chi=2,250,211,129. Plain EF alone is about 3.16 GB;
the run-head and length bit counts and both maps must be measured from the
466 run artifact before any 6--8 GB claim. A 1-bit/run increase in either map
costs another 0.342 GB at 466. Query latency must be measured separately
for count and locate-heavy MEM workloads after the toehold port; the current
SXI1 LF-walk timings cannot stand in for phi move timings.

## Publication gate

Do not link an SXI2 file until the writer, loader and both `xsa mems` and
`xsa serve` ports pass: byte-identical output against SXI1 for a nonempty
multi-occurrence battery on yeast235, a k10 slice and remapped pile-frag;
exact chi stream and count; HTTP response parity; format differential and
malformed-member rejection; per-member sizes; and timed count/locate-heavy
comparison. None of these SXI2 gates has run in this worktree. This design
therefore blocks publication rather than endorsing an incomplete container.
