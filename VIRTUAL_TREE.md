# Virtual suffix tree: existing-container preflight fails

2026-11-10 UTC. Read-only study, based on origin/main
`8939896bfea62effa17e7467dadb5cdd852afdf0`. No product changes.

**The suffix-tree observation is correct; the asserted storage premise is not.**
This file does not contain an SA column or an LCP column. In particular m7 is
exactly a **256-byte permutation**, not a slice of LCP. Consequently the requested
LCP-scan implementation and its timings cannot be measured from this container.
The requested corpus is also currently absent. This is an audited negative
preflight, **not a completed 100 GB performance study**. Unknown measurements
below are explicitly unknown, rather than synthetic performance numbers.

Read `bit6/SXI_FORMAT.md` (the repository's SXI_FORMAT.md), the header of
`xsa/src/mem.inc.rs`, and the prior `CHI_TRANSITIONS.md`. The latter is absent
on this origin/main; read the copy at
`/home/erikg/chi-transitions/CHI_TRANSITIONS.md`, worktree HEAD
`acad3d5d13efea5ae0c10dfe59cd469b24420fe2`, document SHA256
`2882f9e15bb4536a70d597f4d58f20e633300c13d0a8f3f65fe2562592c59466`.
It already identifies the same missing-column issue. Its historical performance
measurements are not rerun or represented as measurements of LCP scanning here.

## What was measured

The prototype opens the named 63,163,489,696-byte file read-only, parses its v6
header and directory, verifies header/directory CRC, reads m7 and verifies its
CRC and permutation property. It does not validate all 63 GB of member payloads.
Full-SA/LCP absence is established by directory inspection **and codec source
semantics**, not inferred solely from the names of members.

| Member | Codec | Payload bytes | Actual role |
|---|---:|---:|---|
| m1 | 114 | 29,738,280,736 | Compressed RLBWT, C, block directory |
| m4 | 4 | 12 | Empty record anchors |
| m5 | 105 | 12,194,502,226 | 24,185,339,204 chi witness positions |
| m7 | 7 | 256 | Original-byte to indexed-symbol permutation |
| m10 | 110 | 0 | LF starts derived from rank directories |
| m11 | 111 | 245,168,752 | 30,646,092 sampled run-tail SA values |
| m12 | 112 | 248,999,568 | BWT row-to-run directory |
| m13 | 115 | 20,736,537,752 | Per-symbol rank directories |

There are 100,000,000,001 suffix rows and 31,381,597,694 BWT runs.
At file offset 12,194,502,632, m7 contains `sigma[original_byte]`; its index is
an integer from 0 through 255. There is no mapping `m7[SA_rank]` and no LCP
slice directory. m7 SHA256:
`0ce35ac950b471c0da0e9a7773a6d82871d53d5de5a496bb9ffda6078b896780`.
See `xsa/src/qlite.rs` remap loading, m12/m13 parsing, and `locate_steps`;
`bit6/SXI_FORMAT.md` describes their codecs. Producer LCP bands are transient
(`xsa/runtime/bit6/xsa_finish.cpp`, `lcpCol.finishBands()`); publication does
not preserve them as m7. SA samples are not a random-access full SA column.

`/mnt/nvme2n1/erikg/pile-head100.txt` does not exist at measurement time;
a filename search under `/mnt/nvme2n1/erikg` found no copy with that name.
No corpus was recreated or container rebuilt.

## Exact child algorithm and exactness

For conventional lexicographic SA and
`LCP[i] = lcp(T[SA[i-1]:], T[SA[i]:])`, a context P of depth d has contiguous
interval [l,r). Split at each interior i where `LCP[i] <= d`. Each resulting
maximal run [a,b) has one next-byte label `T[SA[a]+d]` and count b-a. A suffix
ending at depth d gets a separate END label, not an ordinary byte. Handle
singleton intervals too. A byte permutation changes child order; inverse-map
indexed labels if accessing indexed text. Implicit unary depths on compressed
edges still require the depth and access to the edge's next byte.

Thus contiguity is sufficient **if** these columns and label access exist.
LCP alone gives widths, not next-byte labels. Enumerating k children costs
O(r-l) LCP inspections plus k label accesses. Prototype `children()` implements
this formula using explicitly supplied SA/LCP/text; no product integration.

Reference correctness: 100 deterministic synthetic contexts match independent
next-byte histograms, plus root/EOF and singleton checks. These are algorithm
checks only. **Real 100 GB exactness: 0 contexts checked; >=50 requirement
unmet.** No actual-container parity or exactness percentage is claimed.

## Cost table

N/M means not measurable by the requested method with the available inputs;
it does not mean zero cost, timeout, or a lower bound measured on hardware.

| Interval width | Real contexts timed | Children | Enumeration p50 us | Enumeration p99 us |
|---|---:|---|---|---|
| 1 | 0 | N/M | N/M | N/M |
| 10 | 0 | N/M | N/M | N/M |
| 1k | 0 | N/M | N/M | N/M |
| 100k | 0 | N/M | N/M | N/M |
| 10M | 0 | N/M | N/M | N/M |
| 100M+ | 0 | N/M | N/M | N/M |
| `the `, claimed 313M | 0 | N/M | N/M | N/M |

The 313M count is supplied by the task, not independently verified here.
If it holds, one enumeration inspects 312,999,999 interior LCP entries.
A hypothetical 4-byte LCP would read about 1.252 GB per enumeration; at
150,000 enumerations/s that is 187.8 TB/s aggregate just for LCP. Even a
1-byte-per-row representation requires 46.95 TB/s, before decoding or labels.
These are arithmetic traffic models, **not timing measurements**. Repeated
uncached wide scans are not a viable serving plan at this target; caching
changes the operation being measured. There is no measured crossover W.

## Rolling ladder

| Workload | Measured positions | us/position | positions/s/core | Meets 150k/s/node? |
|---|---:|---|---|---|
| 1024-byte rolling ladder using LCP child enumeration | 0 | N/M | N/M | Not demonstrated |

The recurrence needs up to 1024 child operations per position on maintained
interval/depth states, with empty states short-circuiting. It cannot call the
missing LCP accessor. A timing loop over generated arrays would not establish
actual-container locality or rates. Query-side MEM right maximality in
`mem.inc.rs` searches the query's longer string via **prepend/rank** steps;
it does not supply the requested index-side append from an existing interval.

This node is a dual-socket EPYC 7713, 128 physical cores / 256 SMT threads.
The target allows 6.667 us/position on one core or 853.333 us/core/position at
perfect 128-core scaling. Neither rate was demonstrated for this operation.

## Threshold and size model

Chi=24.185339204 G counts witness text positions, not suffix-tree nodes.
Scaling a sample of chi positions by chi does not estimate a uniform node
population. An occurrence-weighted context sample would likewise be biased.
No node census or valid inclusion probabilities exist here, so the requested
node-threshold extrapolation cannot honestly be supplied.

Define K_W as the number of actual branching nodes above width W and E_W as
their total outgoing degree. An illustrative **payload-only** sparse table
stores `(l,r,depth,pool_offset)` per node and `(byte,child_l,child_r)` per edge.
With five-byte integers at 100 GB and six-byte integers at pile scale this is
20 K_W + 11 E_W bytes and 24 K_W + 13 E_W bytes respectively. Depth fits
because only <=1024 is needed. This excludes lookup indexing, alignment,
checksums, END handling and access along unary edges. Wide-node edges alone
do not solve narrow-node navigation in the present container.

| Materialisation | Measured nodes above W | Added bytes at 100 GB | Added bytes at pile, chi about 317G |
|---|---|---|---|
| None | Not applicable | 0; requested navigation unavailable | 0; requested navigation unavailable |
| Children for W=1k | Unknown | 20 K_1k + 11 E_1k | 24 K'_1k + 13 E'_1k |
| Children for W=10k | Unknown | 20 K_10k + 11 E_10k | 24 K'_10k + 13 E'_10k |
| Children for W=100k | Unknown | 20 K_100k + 11 E_100k | 24 K'_100k + 13 E'_100k |
| Children for W=1M | Unknown | 20 K_1M + 11 E_1M | 24 K'_1M + 13 E'_1M |

Primes denote a separate pile population, not measured scaled counts.
If one **assumes** identical node/degree distributions and K,E proportional
to chi, the multiplier is 317/24.185339204 = approximately 13.107. That is an
unsupported sensitivity assumption, not an estimate. Actual node counts can
only come from a context/tree census, such as a reconstructed LCP stream.

For scale only, uncompressed uint64 SA plus uint32 LCP at 100 GB is
1,200,000,000,012 bytes, plus retained text for labels. uint32 LCP needs an
escape scheme or clipping at the 1024 depth limit. This is an example
representation cost, not a minimal storage lower bound; compressed indexes
can be far smaller.

## Verdict and residual risks

**The proposed ladder is not available as an SA+LCP scan from the existing
container.** The first wall is access to rank-indexed LCP and next-byte labels,
before scan bandwidth can even be benchmarked. There is no measured per-position
rate or evidence for 150k/s/node. This finding does not prove that every
no-new-persistent-structure algorithm is impossible: reconstructing data in
memory or repeated index searches are different algorithms with different
costs, and were not measured here.

The minimum missing *capability* is exact child-boundary discovery plus a
next-byte accessor and interval/depth state. To use the specified algorithm,
reconstruct/provide LCP and suffix/text access; these could be transient for a
study. For serving, add navigation (e.g. a compressed tree with edge-label
access, or another direction-appropriate index) or derive it at runtime.
No defensible minimum byte count or W follows from these receipts. Materialising
only wide children is insufficient while narrow LCP/label access is missing.
Selecting a minimum structure requires a new census and kernel measurement.

Residual risks: no >=50 real-context oracle, width timings, ladder timings,
threshold census, or multicore measurement. Missing source text and LCP are
independent blockers. The synthetic checks establish only the grouping formula.
No claim is made about a new structure's eventual performance or size.

## Reproduction and scope

```sh
git fetch origin
git worktree add -b study/virtual-tree /tmp/virtual-tree-study origin/main
cd /tmp/virtual-tree-study
python3 xsa/tests/virtual_tree/probe.py \
  /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 \
  /mnt/nvme2n1/erikg/pile-head100.txt \
  > /tmp/virtual-tree-audit.json
diff -u xsa/tests/virtual_tree/audit.json /tmp/virtual-tree-audit.json
```

`audit.json` is the actual receipt; no product path is modified. The probe uses
only Python's standard library and performs bounded reads. Its absence findings
are deliberate preflight output, not simulated workload timing. Intercom
`contact_supervisor` was unavailable: tool metadata searches for
`contact_supervisor`, `supervisor`, and `intercom` returned no matches. No
coordination decision was received; this document preserves the blockers
without silently replacing the requested experiment with a different one.
