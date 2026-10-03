# VLB evaluation lane — paper notes (Phase 1)

Paper: "Adaptive encodings for small and fast compressed suffix arrays",
Diego Díaz-Domínguez and Veli Mäkinen, Univ. of Helsinki.
arXiv:2602.17201v1 [cs.DS] 19 Feb 2026. ACM Trans. Algor. format, 28 pp.
Full text extracted to `paper-fulltext.txt` (source: arXiv PDF, 1.16 MB, 28 pp).
Fetched 2026-10-03; abs page confirmed title/authors.

## (a) Space: asymptotic + empirical

- **No asymptotic bound in (r, n) exists.** Section 9 explicitly lists
  "derive space bounds that reflect the adaptivity" as an OPEN problem; the
  apparent `σ(n/ℓ)` worst-case Z-array overhead is only "moderated by our
  sampling strategy" in practice. What IS proved: query time
  O(m(log_f(ℓ/w)+w)) for count (Thm 3.2), access O(log_f(ℓ/w)+w).
- Empirical space (Fig. 6 x-axes, bits/text symbol):
  - run-length BWT (vlbt-bwt): BAC 0.2–1.0, COVID 0.03–0.12, HUM 0.27–0.33,
    KERNEL 0.1–0.4 bps over block sizes ℓ=4^6..4^9 (w=64, f=4).
  - full CSA (vlbt-sri-va): BAC 0.5–2.0, COVID 0.10–0.25, HUM 0.4–1.6,
    KERNEL 0.2–0.8 bps over ℓ=4^6..4^9 × s=4,8,16,32.
- Space breakdown (Sec 8.6): encoded BWT runs = 36–82% of vlbt-bwt; SA
  samples (SAh + φ⁻¹ tree) = 21–71.09% of vlbt-sri-va; VLB-tree overhead
  6.27–21.72% (KERNEL 50%). Root Z arrays up to 29% of vlbt-bwt at ℓ=4^6
  on KERNEL; reduced by Sec-5 sampling + larger ℓ.
- Subsample lever: s: 4→32 shrinks vlbt-sri-va by 3.02–29.18% (max HUM);
  on HUM the s lever alone gives ~51% total size reduction.
- vs competitors: vlbt-bwt 1.06–4.6x smaller than fbb (fbb 12.77–17.77%
  smaller on HUM — the one space loss); up to 2.07x smaller than mn;
  move-count needs 2.55–8.33x MORE space. vlbt-sri-va ≈ sri-va space
  ("broadly comparable", "differences modest"); up to 2.28x smaller than ri.

## (b) Locate cost per occurrence

- Reported as µs per reported occurrence (50k patterns of length 105;
  locate restricted to patterns with <50k occurrences). Fig. 6C y-ranges
  are ~0.2–2.25 µs/occ across datasets/variants.
- Ratios: vlbt-sri-va is 1.57–9.64x faster than ri, 1.3–5.38x faster than
  sri-va at comparable space; move-loc is 2.38–3.57x faster than
  vlbt-sri-va at 2.23–5.63x the space.
- Mechanism: toehold SA[sp] during backward search (≤ s LF steps recovery,
  Eq. 4), then φ⁻¹ chain (predecessor in T_φ + valid-prefix check V/M;
  LF steps only in invalid areas). Count-vs-locate asymmetry: locate
  speedups (1.3–5.38x vs sri-va) are smaller than count speedups
  (2.37–6.81x) because "decoding consecutive values remains cache
  inefficient in both cases" — their own admitted open problem.

## (c) Self-index?

**Yes.** vlbt-sri-va = VLB-tree of BWT (rank/headrank/msucc) + SAh in
leaves + VLB-tree T_φ for φ⁻¹. count(P) and locate(P) are answered with NO
access to the text. "a small fully-functional compressed suffix array".
(The vlbt-bwt variant is count-only; move comparisons for locate use
move-loc which carries r-samples.)

## (d) The state-valid sampling — precise definition

Two distinct mechanisms; do not conflate:

1. **Sec. 5 VLB-tree Z-array sampling — the actual "state-valid" result.**
   - Valid backward-search states: `S =` the set of SA ranges that
     backwardsearch(P) may visit for any pattern P occurring in S. The
     paper's footnote: S = ranges associated with the **nodes of the
     suffix tree** of S. A triple (sp_l, ep_l, P[l-1]) is *valid* iff
     P[l-1] occurs in L[sp_l..ep_l] (iff P[l-1..m] occurs in S).
   - What is sampled: the root-child **routing arrays Z** (u2.Z[c] =
     distance to closest right sibling whose fragment contains c), NOT SA
     samples. Lemma 5.1: root child with fragment L[p..q] needs Z entries
     only for symbols in Σ(a,p-1) ∪ Σ(q+1,z), where (a,b) = lmo(p,q) is
     the *leftmost overlap* (the (s,e)∈S with s≤p≤e≤q, min s, tiebreak
     max e) and (y,z) = rmo(p,q) the *rightmost overlap* (p≤s≤q≤e, max e,
     tiebreak min s). I.e. only the alphabets of the BWT strips covered
     by the maximal suffix-tree ranges overlapping the fragment's ends.
   - Correctness relaxation: rank/succ/headrank may return wrong values
     for (i, c) pairs that never arise from an occurring pattern; for
     non-occurring P the search still returns an empty interval. Pattern
     matching over occurring patterns is exactly correct.
   - Cost relieved: root-level σ(n/ℓ)-bit worst case — a VLB-tree-specific
     routing overhead. **It does not subsample SA samples.**
2. **Sec. 7 SA-sample subsampling = the sr-index scheme** (Cobas et al.):
   discard SAh[p_a] when p_{a+1} - p_{a-1} ≤ s (text-space density rule);
   validity of φ⁻¹ becomes area-based (fully/partially valid B intervals,
   V/M structures); toehold recovery ≤ s LF steps. This is where the
   samples 1.4–2.4 TB → subsampled reduction lives — already banked by the
   size-survey lane as sr-index subsampling; VLB reorganizes it for cache
   locality but adds no new sample-space lever.

## (e) Public implementation

Yes: **github.com/ddiazdom/VLBT** — "A small and fast encoding for BWTs and
suffix arrays" (verified live 2026-10-03, public repo id 747397522).
Contains the library + `test_suite` CLI used for the benchmarks.

## (f) Benchmarks and measured wins

Datasets (Table 1; DNA corpora include reverse complements):

| Dataset | Size | n/r | Notes |
|---|---:|---:|---|
| BAC | 33.12 GB | 116.77 | 30 AllTheBacteria species |
| COVID | 67.41 GB | 940.49 | 4,494,508 SARS-CoV-2 genomes |
| HUM | 41.24 GB | 61.82 | 40 HPRC assemblies |
| KERNEL | 54.45 GB | 263.11 | 2,609,417 kernel versions |

50,000 random patterns of length 105 per dataset. AMD EPYC 7713, 1 TB RAM,
gcc 13.3 -O3, SSE4.2 SIMD. All BWTs/samples from bigBWT.

- count, run-length BWTs: vlbt-bwt 1.15–2.3x faster than fbb; 2.01–5.55x
  faster than mn at up to 2.07x less space; move-count 2.56–4.54x faster
  than vlbt-bwt but 2.55–8.33x larger.
- count, CSAs: ri 3.22–8.14x slower and up to 2.28x larger than
  vlbt-sri-va; sri-va 2.37–6.81x slower at comparable space; move-loc
  2.63–4.76x faster, 2.23–5.63x larger.
- locate: vlbt-sri-va 1.57–9.64x faster than ri; 1.3–5.38x faster than
  sri-va; move-loc 2.38–3.57x faster, 2.23–5.63x larger (BAC worst 5.34–5.63x).
- Cache (L1D misses/query symbol, Fig. 7): fbb 1.54–4.35x and mn
  3.41–6.33x more misses than vlbt-bwt; ri 4.51–9.84x and sri-va
  4.48–8.46x more than vlbt-sri-va (count); locate: ri 1.85–9.34x,
  sri-va 1.93–5.7x more. The speedups are largely cache-locality effects
  (their claim, backed by these counters).
- Block size ℓ 4^6→4^9 cuts vlbt-bwt space 5.72–39.53% (max COVID).

## Relevance caveats for our container (noted up front, honest)

- Their least compressible corpus is HUM at n/r = 61.8. Our pile regime is
  n/r ≈ 2.8 — 22x more incompressible than anything they test. On HUM
  (their closest case) fbb is already 12.77–17.77% SMALLER than vlbt-bwt:
  the adaptive-tree space win inverts near the compressible end of their
  range, and the pile is far beyond it.
- The Sec-5 state-valid sampling reduces a VLB-tree-specific routing
  overhead (Z arrays); it is not a new SA-sample subsampling. The task's
  framing — "locate only ever starts from backward-search interval
  states, so a sample set covering only reachable states suffices" —
  needs care: the set of interval states for ALL occurring patterns is
  the set of ALL rows (every row is the singleton interval of its own
  context). A non-trivial restriction needs a workload assumption
  (MEM minimal length L, and/or occurrence count ≥ 2). We implement the
  task's stated proxy — intervals of minimal MEM length L — which equals
  rows whose length-L context is shared with a SA-adjacent suffix, i.e.
  rows in intervals of size ≥ 2, i.e. L-mers occurring ≥ 2 times.
- Their locate speedup over sri-va (1.3–5.38x) is cache layout, not
  sampling; the transplantable piece for our member 8 is the B/D
  interleaving (T_φ) idea, not sample reduction.
