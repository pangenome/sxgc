# VLB evaluation — REPORT (Phases 1–3)

Lane: VLB / state-valid sampling evaluation, 2026-10-03.
Paper: Díaz-Domínguez & Mäkinen, "Adaptive encodings for small and fast
compressed suffix arrays", arXiv:2602.17201 (Feb 2026). Everything in this
directory; retained artifacts read read-only; nothing staged or committed;
corpora read only through the established single-forward-pass gates.
Full paper extraction: `paper-notes.md` (+ `paper-fulltext.txt`).

## TL;DR verdict row

| Question | Answer |
|---|---|
| Warm-form size with state-valid sampling (samples 1.4–2.4 TB → ?) | **No new lever. ~0.35–1.6 TB stands (the already-banked sr-index subsampling).** The paper's "state-valid sampling" (Sec. 5) shrinks the VLB-tree's root *routing* arrays (Z), not SA samples; vlbt-sri-va's sample-space reduction is exactly Cobas et al.'s sr-index subsampling (parameter s, toehold recovery ≤ s LF steps, V/M valid areas). VLB's warm-form value is *layout/cache* (interleaved φ⁻¹ tree), not sampling. |
| Lite locate speedup (walks 3.1k mean → ?) | **2.2x at identical anchor count; ~5.7x at 2.8% of the lite artifact; ~19x at ~4%** — but the win is ORBIT-AWARE anchor placement, not state-validity. State-validity is empty on pangenomes (V covers 96–98% of rows) and hazardous on text (singletons are the majority of real starts at long L). Measured on pile-frag v5, all positions text-verified. |
| Member 1 (414 GB pile core) → VLB tree? | **NO.** VLB's leaf encoding is byte-aligned vbyte (≥8-bit lengths + ~5-bit symbols ≈ 13–16 bits/run) vs our 7.2 bits/run, and its satellite overhead (18–64% on their corpora, worst on their most incompressible) is maximal at n/r = 2.85 where no superblock merging ever happens. Expected pile BWT ≈ 2.5–4x member 1 (~1–1.6 TB vs 414 GB) with no compensating speed win. Their own HUM (n/r=61.8, their least compressible) already loses to fixed-block boosting by 12.77–17.77%; the pile is 22x more incompressible than HUM. |

## Phase 1 — paper (details in paper-notes.md)

(a) **Space**: no asymptotic bound exists (their Sec. 9 lists it as open);
empirical 0.03–0.33 bps for vlbt-bwt and 0.10–1.6 bps for vlbt-sri-va
across their four corpora; runs = 36–82% of vlbt-bwt; SA samples 21–71% of
vlbt-sri-va. (b) **µs/occurrence**: locate reported per occurrence; 1.57–
9.64x faster than ri, 1.3–5.38x faster than sri-va; move-loc still 2.38–
3.57x faster than VLB at 2.23–5.63x space. (c) **Self-index: yes** (count
+ locate with no text access). (d) **State-valid sampling**: the valid
states are the suffix-tree node ranges; what is sampled is the root-child
Z routing arrays, kept only for the alphabets of the leftmost/rightmost
suffix-tree overlaps (Lemma 5.1 lmo/rmo) — NOT SA samples. (e) **Public
implementation**: github.com/ddiazdom/VLBT (C++, BSD-3, live, pushed
2026-08; test_suite + CLI). (f) **Benchmarks**: BAC/COVID/HUM/KERNEL
(33–67 GB, n/r = 61.8–940.5), 50k patterns of length 105; wins as above,
largely cache effects (L1D miss counts 1.9–9.8x lower than ri/sri-va/fbb/mn).

**Regime mismatch (honesty)**: their least compressible corpus is HUM at
n/r = 61.8. Our pile is n/r ≈ 2.85 — 22x more incompressible than anything
they test; even yeast235 (n/r = 33.1) is below their range. Every space
number in the paper is from a regime we do not operate in.

## Phase 2 — state-valid anchor sampling prototype (`vlb_stateval.cpp`)

### Setup

Exact reconstruction of the member-11 locate semantics over the retained
v5 artifacts (read-only): text by inverse BWT (yeast: BYTE-EXACT vs
syng/yeast.fa, records reversed, one forward pass), SA[row] for all rows
via anchor-segment walks, V_L = rows whose length-L context is shared with
a SA-adjacent suffix (= rows in intervals of size ≥ 2 of L-mers = the exact
set of MEM-locate starts of minimal length L *that occur ≥ 2 times*), the
LF-orbit gap structure between run tails, greedy anchor placement bounding
the walk from covered rows by D, and a text-verified locate benchmark.
Gates all green on both corpora: member-11 anchor values == SA[tailrow]
(98,541 / 388,402 checks), sum(SA) == n(n−1)/2, 0 SA-parity and 0
context mismatches on every benchmark row, and the orbit machinery
reproduces the banked lane numbers EXACTLY (yeast 221,572.8/6,516,844;
pile 3,050.6/134,858).

Honest framing (tested, not assumed): the set of backward-search interval
states over ALL occurring patterns is ALL rows — every row is the
singleton interval of its own context. A non-trivial "state-valid" set
requires the workload restriction (MEM length ≥ L, occurrences ≥ 2);
singleton starts stay correct under any placement (the orbit is a single
cycle) but lose the D-bound.

### The central measurement

| | yeast235 (n/R=33.1) | pile-frag (n/R=2.7) |
|---|---:|---:|
| \|V_20\| / n | 98.50% | 28.31% |
| \|V_39/31\| / n | 97.70% (39) | 11.64% (31) |
| \|V_100\| / n | 95.84% | 4.03% |
| member-11 walk mean over V_L | 224.7k / 226.3k / 229.7k | 3,763 / 4,882 / 6,856 |
| anchors for D-bound on V_L at member-11 count | 98,540 (D*=31,400) | 388,367 (D*=2,762) |

**State-validity itself buys almost nothing.** On yeast V_L ≈ 96–98% of
all rows (pangenome LCPs are huge), so covering V_L is covering the world:
at equal anchor count the state-valid placement (mean walk 43,160) is
statistically identical to the workload-independent all-rows placement
(42,844). On pile-frag V_20 is 28% of rows yet needs only 0.7% fewer
anchors than all-rows at D=4,096 (262,659 vs 264,620): valid rows are
clumped on the orbit, but an anchor's D-shadow covers clumps and stragglers
alike. At L=100 state-validity needs 73% fewer anchors (71,941) — and then
strands the singleton starts, which are 88% of the real benchmark starts:
measured locate from them degraded 10x (mean 33,893 steps vs member-11's
3,318). **The state-valid restriction is either empty or harmful.**

**What actually wins — orbit-aware anchor placement** (replace "every
1024th run" with "runs whose tails bound the LF-orbit walk"; same u64
tail-SA values, same count):

| corpus | anchors | mean walk | max walk | measured latency* |
|---|---:|---:|---:|---:|
| yeast235 member-11 (banked) | 98,541 | 221,573 | 6,516,845 | 112–124 ms/locate |
| yeast235 orbit-aware, same count | 98,538 | **42,844 (5.2x)** | 2,164,858 (3.0x) | **21.4 ms (5.2x)** |
| pile-frag member-11 (banked) | 388,402 | 3,051 | 134,859 | 1.7–2.2 ms/locate |
| pile-frag orbit-aware, same count | 388,380 | **1,415 (2.2x)** | 97,037 (1.4x) | **0.65 ms (2.65x)** |

*31 threads, same-run ratio; steps are the deterministic metric.

Anchor-space tradeoff (all-rows, workload-independent):

| pile-frag D | anchors | values+bitmap vs 2.05 GB lite | mean walk |
|---:|---:|---:|---:|
| 256 | 4,253,158 | 34 MB + 50 MB ≈ **4.1%** | **158 (19.3x)** |
| 1,024 | 1,061,005 | 8.5 MB + 50 MB ≈ 2.8% | 537 (5.7x) |
| 4,096 | 264,620 | 2.1 MB + 50 MB ≈ 2.5% | 2,065 (1.48x) |
| same count as member-11 | 388,380 | 0.15% (as today) | 1,415 (2.16x) |

| yeast235 D (V_20 ≈ all-rows here) | anchors | mean walk |
|---:|---:|---:|
| 4,096 | 655k (6.6x member-11) | 33,900 (6.6x) |
| 65,536 | 48,855 (0.50x) | 56,475 (4.0x) |
| 262,144 | 12,911 (0.13x) | 141,084 (1.6x) |

This **dominates the skiplist lane's safe-jump pointers on the
space–time plane**: ~4% of the lite artifact buys ~19x here vs their 47%
for 5.8x (the two compose — jumps would sit on better-placed anchors).
Extrapolated to the real pile (R=4.6e11, lite floor 500–580 GB): member-11
anchors are 3.6 GB (0.6–0.7%); D=256-class density (1 anchor/103 runs)
≈ 36 GB values (~6–7% of lite) for a projected ~19x locate — extrapolation
flagged: pile-frag (n/R=2.7) is a fragment of the pile (n/R=2.85) and the
orbit structure should transfer, but the constant was measured at 1 GB
scale, not 1.3 TB.

**Caveats (honest)**: (1) the placement itself needs orbit information;
this lane computed it with O(n) measurement passes (lane precedent), which
is NOT a lawful build step at pile scale — production placement needs
sampled orbit-gap estimation (the skiplist lane's `entry`-mode pattern) or
an r-space invariant; open problem. (2) Singleton starts keep correctness
but not the bound under any partial placement; the all-rows variant has no
hazard. (3) At tiny D the forced floor is the max orbit gap between
consecutive run tails (yeast 2.16M, pile-frag 97k steps) — the mega-run
satellite stretches bound what any tail-anchored scheme can do, exactly as
the skiplist lane found.

## Phase 3 — the three verdicts (arithmetic)

1. **Warm form, samples 1.4–2.4 TB → ?** → **0.35–1.6 TB, unchanged.**
   VLB's Sec. 5 state-valid sampling reduces a VLB-tree-specific routing
   overhead (Z arrays, up to 29% of vlbt-bwt on KERNEL at small ℓ) — a
   structure we would not adopt (see 3). The SA-sample reduction in
   vlbt-sri-va is sr-index subsampling, already banked. If we want the
   warm form faster (not smaller), the transplantable VLB idea is the
   interleaved φ⁻¹ tree (T_φ: B and D colocated, valid-prefix metadata in
   leaves): their 1.3–5.38x locate win over sri-va is cache layout, and
   member 8 (φ, 1.4–2.4 TB pre-subsampling) is where that layout would
   apply. That is a re-encoding proposal for a future lane, not a space
   lever.
2. **Lite locate, 3.1k walks → ?** → **1.4k at zero extra space (2.2x);
   ~160 steps at ~4% of the lite artifact (~19x)** — via orbit-aware
   anchors (Phase 2 table), not via state-validity. Text-verified: every
   benchmark position checked against the reconstructed SA and the
   pattern bytes (0 mismatches); answer parity flat vs state-valid on
   7,000+ located rows per configuration.
3. **Member 1 → VLB tree? NO.** member 1 at pile = 414 GB = 7.2 bits/run
   (R=4.6e11). VLB leaf runs: byte-aligned vbyte-style packing, control
   byte + ≥1 byte length per run, symbol bits on top → ≥13–16 bits/run on
   mean-length-2.85 runs, i.e. ~1.8–2.2x member 1 *before* satellite data.
   Satellite (A/O/X/P/N/Z + prefix counts): 18–64% of vlbt-bwt on their
   corpora, worst at their incompressible end; at n/r=2.85 every ℓ-block
   (ℓ = 4^6..4^9) contains 1.4k–92k runs ≫ w=64, so the tree subdivides to
   full depth everywhere and no superblock merging ever fires — overhead at
   the top of their range. Expected pile VLB BWT ≈ 1–1.6 TB vs 414 GB.
   Their speed win comes from superblock locality in compressible regions,
   which do not exist at n/r=2.85. Their own HUM (n/r=61.8) already shows
   fixed-block boosting 12.77–17.77% smaller than vlbt-bwt. **Member 1
   stays.** (For satellite-heavy pangenomes like yeast/k10 the arithmetic
   would be closer, but member 1 is not the cost driver there either.)

## What survives from the paper for our line

- The **layout insight** (colocate what a query touches; their cache-miss
  counts attribute the wins to locality) → candidate member-8
  re-encoding (T_φ-style interleave), future lane.
- The **state-valid *principle*** (correctness only along reachable
  states) is real but, for anchor sampling, resolves to: reachable states
  = all rows; the productive transplant is the *orbit-aware* placement
  that the same "where do walks actually go" reasoning produces.
- Their construction bottleneck note ("suffix-array samples at scale
  remain limited") matches our v5-provenance constraints.

## Reproduction

```sh
# in this directory; artifacts read-only; ~27 GB RAM, <=31 threads
./vlb_stateval /mnt/nvme3n1/erikg/sxi2-v5/yeast235.sxi2 full \
  --Ls 20,39,100 --Ds 1024,4096,16384,65536,262144,1048576 \
  --queries 1000 --threads 31 --text-cache /tmp/vlb-yeast235.txt \
  --src-fa /mnt/nvme3n1/erikg/sxgc-yeast/syng/yeast.fa \
  --out-anchors yeast235.anchors.bin
./vlb_stateval /mnt/nvme3n1/erikg/sxi2-v5/pile-frag.sxi2 full \
  --Ls 20,31,100 --Ds 256,1024,4096,16384,65536,262144 \
  --queries 1000 --threads 31 --text-cache /tmp/vlb-pile-frag.txt \
  --out-anchors pile-frag.anchors.bin
# logs: yeast235.run.log, pile-frag.run.log, pile-frag.run2.log
# selftests (rank brute / LF bijection / L[i]==T[SA[i]-1] for all rows):
./vlb_stateval CORPUS selftest
```

Deviation note: benchmark patterns are drawn from the indexed text itself
(length-L contexts without separators) rather than the fixed
v7-model probe file, to stay inside the container's byte domain on
pile-frag (no byte-permutation handling needed) and to guarantee occurring
patterns; the witness-locate lane's verification *pattern* (recovered
positions text-verified, source byte-compare) is preserved, and on yeast
the reconstructed text itself is byte-exact against the source corpus.

Bug history (all caught by gates before conclusions): interval-interior
row sampler overshoot; segment order ≠ orbit order; per-gap accumulators
not reset at gap close. Gates added: orbit-order monotonicity, wrap record,
V_L partition, banked-lane cross-check.
