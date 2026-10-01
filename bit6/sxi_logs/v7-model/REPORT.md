# v7 witness-restricted model — 2026-10-01

Modeling only. No artifact was modified or published, no corpus bytes were read, and no
production builder was implemented. The source artifacts are the retained SXI2 v4
publications at `/mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/`. The corresponding v5
v3 copies have identical member 1 (runs), 5 (EF chi), and 8 (phi) byte lengths,
counts, and CRCs on all three corpora; see `header_crosscheck.json`.

## 1. Exact census

The model decodes every EF chi position, every compact phi edge, and every run.
For cyclic SXI coordinates, a witness at text position `x` corresponds to
`SA = 0` when `x = 0`, otherwise `SA = n - x`. Each phi edge `(u,v,run)`
supplies the tail SA of `run` (`u`) and head SA of `(run+1) mod R` (`v`).
The decoded chi bitmap hits every witness at one of those edges: `unmapped_chi=0`
in all three result files. A run containing a witness is therefore exactly a
run with a witness at its head or tail.

| Corpus | R | Head-witness runs | Any-witness runs | Witness-free runs | Retained fraction | Optimistic v7 bytes |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 100,905,045 | 54,210,669 | 80,121,608 | 20,783,437 | 0.794030 | 832,094,598 |
| pile-frag | 397,723,010 | 261,733,742 | 295,804,478 | 101,918,532 | 0.743745 | 2,811,696,918 |
| k10 | 1,859,825,862 | 879,364,004 | 1,516,975,271 | 342,850,591 | 0.815654 | 16,997,915,778 |

The theory in `lean/Sxgc.lean` at `witnesses_at_run_edges` says **head or tail**;
the head-only statement was retired after counterexamples. Head-only retention
would miss 31,193,667 yeast, 44,431,023 fragment, and 747,703,253 k10 chi
positions. Chi/R is not the retained-run fraction because a run may carry two
witnesses and a one-row run can be both a head and tail witness. Exact chi/R
from these artifacts is yeast 0.846383, fragment 0.769794, k10 0.874849.
The supplied 466 counts give `2,250,211,129 / 2,739,737,289 = 0.821324`;
there is no 466 SXI2 artifact here for an exact bearing-run census.

## 2. Skeleton and route

The charged skeleton is one bit per **original** run, 64-bit rank counts every
512 runs, and the 2,048-byte C array. This omits select, original run starts,
symbols, and any per-character rank directory; its charged size is optimistic.

| Scale | Bitvector bytes | Rank directory bytes | Charged total bytes |
|---|---:|---:|---:|
| yeast235 | 12,613,131 | 1,576,648 | 14,191,827 |
| pile-frag | 49,715,377 | 6,214,424 | 55,931,849 |
| k10 | 232,478,233 | 29,059,784 | 261,540,065 |
| 466 (supplied R) | 342,467,162 | 42,808,400 | 385,277,610 |
| whole Pile (assumed R = 460 billion) | 57,500,000,000 | 7,187,500,000 | 64,687,502,048 |

The exact candidate routing procedure, using zero-based half-open BWT intervals,
is:

```text
route(pattern):
    (l, r) = (0, n)
    for c in indexed_orientation(pattern):
        for p in (l, r):
            if p == 0: rank_p = 0; continue
            if p == n: rank_p = (n if c == 255 else C[c+1]) - C[c]; continue
            i = original_run_containing(p)           # needs ALL run starts
            s = original_run_start(i)
            if symbol(i) == c and p > s:
                require witness_bit[i] and stored LF_start(i)
                rank_p = LF_start(i) - C[c] + (p - s)
            else:
                j = last ORIGINAL c-run ending at or before p
                if j does not exist: rank_p = 0
                else:
                    require witness_bit[j] and stored LF_start(j)
                    rank_p = LF_start(j) - C[c] + length(j)
            # If a require fails, route STALLS and needs full-index fallback.
        (l, r) = (C[c] + rank_l, C[c] + rank_r)
        if l >= r: return empty
    return (l, r)
```

The bitvector and C array do not implement `original_run_containing`,
`symbol(i)`, or `last ORIGINAL c-run`. Searching only retained c-runs is wrong
when a discarded c-run intervenes. Keeping the complete run table would
restore the first two functions but does not by itself restore the dropped
rank values or phi transitions. Retained LF starts must also be exact values
from the original index; recomputing them over a reduced run order changes
rank. This is the specific point where the proposed small skeleton stalls.
The existing product also reports occurrence coordinates; deleting phi
domains for witness-free runs leaves arbitrary-row locate without a stated
route, even if interval search were repaired.

`covering_given_stream` gives an existential witness for each coverage
requirement. `RunEdgeHit` gives an edge representative for maximal classes.
Neither theorem says that both rank endpoints of every **intermediate**
backward-search interval, for every next character, can use retained runs.
The query audit below directly tests this missing closure condition.

## 3. Query dependency audit

`queries.py` reads only retained gate patterns and retained short query-read
FASTA files. With a fixed seed it makes 1,000 fresh reads per corpus from
12–40 byte slices (25% with one changed byte), plus the stored HTTP gate
pattern. The simulator applies the artifact byte permutation and the product
frontend's indexed orientation. In `--ms` mode it follows the runtime's
matching-statistics extension, shortening, and restart loops, computing exact
intervals with the full run table and auditing **each** backward step. A rank
is available from the candidate only if
the current c-run supplies an interior contribution and is retained, or the
last original c-run before the endpoint is retained; rank 0 and total ranks
are free. The full table is an oracle for this audit, not part of v7.

Categories are mutually exclusive: **full** means all containing runs and
rank dependencies are retained; **partial** means a containing run is absent
but every rank dependency is retained; **broken** means at least one needed
rank dependency is absent. A broken read needs fallback. The simulator
models the matching-statistics interval path used in MEM search; it does not
perform occurrence enumeration or measure a real v7 query runtime.

| Corpus | Queries | Full | Partial | Broken / fallback | Steps | Rank-available steps | Reads with nonzero MS |
|---|---:|---:|---:|---:|---:|---:|---:|
| yeast235 | 1,001 | 0 | 7 | 994 (99.30%) | 43,644 | 31,803 (72.87%) | 1,001 |
| pile-frag | 1,001 | 0 | 0 | 1,001 (100%) | 57,283 | 33,920 (59.21%) | 1,001 |
| k10 | 1,001 | 0 | 22 | 979 (97.80%) | 36,608 | 27,279 (74.52%) | 1,001 |

The oracle-audited rank-route success fractions (`full + partial`) are 0.70%
on yeast, 0% on fragment, and 2.20% on k10; strictly full witness routing
is 0% in all three.
Even these partial cases are not executable with the stated bitvector+C
skeleton because it lacks the original run locator and character predecessor.

The separate `*.result.json` files audit one prefix search per read. The
`*.ms-result.json` files are the primary MEM interval-path evidence above.

The warm v6-2 HTTP medians are yeast 382.275 ms, fragment 0.36154 ms,
k10 3.47365 ms. No v7 latency was measured. For a transparent model let
`p = broken/queries`, `a = restricted-query cost / v6 cost`, and
`d = wasted failed-route cost / v6 cost`. A full-index fallback yields
`T7/T6 = p(1+d) + (1-p)a`. Even the impossible best case `a=d=0` gives
speedup at most `1/p` (about 1.007x for yeast). Any positive failed-route
work pushes latency above that bound. This comparison uses the gate medians
as a common cost scale; the generated probes are not an HTTP timing sample.

| Corpus | Measured warm v6-2 median | Broken fraction `p` | Impossible best-case model `p × T6` | Maximum speedup `1/p` |
|---|---:|---:|---:|---:|
| yeast235 | 382.275 ms | 99.30% | 379.601 ms | 1.007x |
| pile-frag | 0.36154 ms | 100% | 0.36154 ms | 1.000x |
| k10 | 3.47365 ms | 97.80% | 3.39731 ms | 1.022x |

## 4. Space projection and verdict

The requested arithmetic is `f × (S6 - EF_chi) + EF_chi + skeleton`, with
`f = any-witness-runs/R`. All values below are exact integer arithmetic for
that formula, but the formula is an **optimistic size scenario**: it scales
every non-chi byte by `f` and omits the data needed for missing ranks and
original run boundaries.

| Corpus | Published v6-2 bytes | EF-chi bytes | Charged skeleton | Optimistic v7 bytes | Reduction | Full v6 + skeleton floor |
|---|---:|---:|---:|---:|---:|---:|
| yeast235 | 1,010,069,024 | 77,088,381 | 14,191,827 | 832,094,598 | 17.62% | 1,024,260,851 |
| pile-frag | 3,655,580,696 | 144,174,354 | 55,931,849 | 2,811,696,918 | 23.08% | 3,711,512,545 |
| k10 | 20,235,882,024 | 1,252,474,932 | 261,540,065 | 16,997,915,778 | 16.00% | 20,497,422,089 |

Keeping just the **complete compressed run member** so original run starts
and symbols remain available raises these optimistic sizes to 852,475,636
(yeast, 15.60% smaller), 2,903,516,162 (fragment, 20.57% smaller), and
17,408,520,453 (k10, 13.97% smaller). These still omit the per-character
rank support and phi repair needed for a correct standalone index.

Alternatively, preserving an exact `LF_start` per retained run as the route
above requires costs at least 320,486,432 bytes (yeast, 32 bits/value),
1,146,242,353 (fragment, 31 bits/value), and 6,636,766,811 (k10,
35 bits/value) with direct fixed-width packing. Adding **just** those
anchors to the advertised v7 formula makes it 14.1%, 8.3%, and 16.8%
**larger** than v6-2, respectively. A stronger compressed rank scheme may
improve this, but the bitvector+C proposal has no such scheme.

For whole Pile, taking the supplied `R ≈ 460 billion` and scaling the
fragment's measured bytes/run gives a 4.228 TB v6 baseline and 3.252 TB
optimistic v7. Widening packed phi successor and run-ID fields from the
fragment widths to 41 and 39 bits adds 1.150 TB, giving a more realistic
codec-width baseline of **5.378 TB** and optimistic v7 **4.107 TB** (23.63%
smaller). The charged skeleton alone is **64.688 GB**, including 57.5 GB of
bits and 7.188 GB of rank counters. These are extrapolations, not a pile
artifact or a proven upper bound. Retaining the full v6 artifact for fallback
would make the width-adjusted estimate 5.443 TB before any v7 payload.
Direct packed LF starts for the projected retained runs would add 1.753 TB,
raising the model to 5.861 TB (8.97% larger than the projected v6 baseline).
The projected 460-billion-run object also exceeds the current u32 run-ID
limit, so no existing SXI2 reader can publish or query that scale.

### Statement lock for fleet round 3

The end-to-end target, once `V7Image` and the restricted rank procedure above
have concrete Lean definitions, is the following. `fmSearch` must use the
same indexed orientation and half-open interval convention as the runtime.

```lean
-- Statement only; no proof attempted in this round.
opaque V7Image : Type
opaque v7Encode : Text → V7Image
opaque v7Search? : V7Image → List Nat → Option (Nat × Nat)
opaque v7Rank? : V7Image → Nat → Nat → Option Nat
opaque fmSearch : Text → List Nat → Nat × Nat
opaque bwtRank : Text → Nat → Nat → Nat

theorem v7_search_routing_correct
    (T : Text) (hT : positive T = true) (q : List Nat) :
    v7Search? (v7Encode T) q = some (fmSearch T q) := by
  sorry

-- The induction's necessary step-closure obligation:
theorem v7_reachable_rank_total
    (T : Text) (hT : positive T = true) (q : List Nat) (c : Nat)
    (l r : Nat) (hreach : fmSearch T q = (l, r)) (hne : l < r) :
    v7Rank? (v7Encode T) c l = some (bwtRank T c l) ∧
    v7Rank? (v7Encode T) c r = some (bwtRank T c r) := by
  sorry
```

The current bitvector+C+witness-runs candidate **fails** the second target
on the measured workloads. Fleet round 3 must strengthen the representation
or restrict the theorem's query domain before attempting a proof. The
covering theorem and run-edge theorem alone cannot discharge this obligation.

**Verdict:** witness-restricted v7 as proposed is not a standalone search
index. Its best-case byte formula saves 16–23% on the three artifacts; only
the fragment clears 20%. The rank skeleton is incomplete and almost every tested query needs a
discarded run. Direct exact LF anchors erase the space gain on every measured
artifact; keeping v6 for fallback also makes the published object larger.

## Reproduction and limits

`model.cpp` is a read-only mmap decoder and audit; `queries.py` generates
the deterministic probe files; `projection.py` writes `projection.json`.
The commands used are:

```sh
python3 bit6/sxi_logs/v7-model/queries.py
g++ -O3 -std=c++17 bit6/sxi_logs/v7-model/model.cpp -o /tmp/v7-model
/tmp/v7-model /mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/yeast235.sxi2 yeast235 bit6/sxi_logs/v7-model/yeast235.queries.tsv > bit6/sxi_logs/v7-model/yeast235.result.json
/tmp/v7-model /mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/pile-frag.sxi2 pile-frag bit6/sxi_logs/v7-model/pile-frag.queries.tsv > bit6/sxi_logs/v7-model/pile-frag.result.json
/tmp/v7-model /mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/k10.sxi2 k10 bit6/sxi_logs/v7-model/k10.queries.tsv > bit6/sxi_logs/v7-model/k10.result.json
/tmp/v7-model /mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/yeast235.sxi2 yeast235 bit6/sxi_logs/v7-model/yeast235.queries.tsv --ms > bit6/sxi_logs/v7-model/yeast235.ms-result.json
/tmp/v7-model /mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/pile-frag.sxi2 pile-frag bit6/sxi_logs/v7-model/pile-frag.queries.tsv --ms > bit6/sxi_logs/v7-model/pile-frag.ms-result.json
/tmp/v7-model /mnt/nvme3n1/erikg/sxi2-v6-2-b25eaecb/k10.sxi2 k10 bit6/sxi_logs/v7-model/k10.queries.tsv --ms > bit6/sxi_logs/v7-model/k10.ms-result.json
python3 bit6/sxi_logs/v7-model/projection.py
```

The audit neither benchmarks a restricted implementation nor proves a lower
bound on all possible skeletons. Query probes reuse a small retained read
set, and only the primary orientation is audited for DNA. The negative
result applies to this proposed skeleton and route, and the space formula
must not be reported as an achievable published artifact size.
Runs used one worker thread each; measured peak RSS was about 2.4 GB for
yeast, 6.1 GB for fragment, and 37.7 GB for k10. No corpus file was opened.
