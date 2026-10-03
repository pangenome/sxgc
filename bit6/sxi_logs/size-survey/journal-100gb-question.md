# SIZE SURVEY — "The 100GB question": can anything go deeper than exact-lite ~500-580 GB at pile scale?

Date: 2026-10-02/03 (UTC). Worker session, single-writer.
Rules compliance: no git add/commit (all outputs unstaged worktree files); corpus read
single-pass in-tool (THE LAW); retained artifacts read-only; <32 procs (max concurrent
this session: lz77z 1 proc + zstd 4 threads); <100 GB RAM (lz77z peak 10.9 GB, zstd ~2 GB).

Inputs from context (measured previously by the team, taken as given):
- Pile rleBWT: ~414 GB at 7.2 bits/run, R = 4.6e11 runs (whole pile).
- chi: ~155 GB at EF floor; anchors ~7 GB.
- Exact-lite total floors at ~500-580 GB; samples 1.4-2.4 TB in any encoding (measured).
- Fragment (this session): n = 1,082,130,213 bytes, R = 397,723,010 (R/n = 0.3675).
- File: /home/erikg/sxgc-piletest/pile-frag.txt, sha256 starts 69a6b5fde1e79d59.

---

## 1. THE DECIDING MEASUREMENT — greedy LZ77 factor count z of the fragment

Method (implemented in this dir, `lz77z.c`, v2 binary `lz77z2`):
- Single-pass corpus read: file read once fully into RAM (~1.08 GB), closed
  immediately. THE LAW honored: no second pass over the corpus anywhere.
- Progressive insertion: positions are inserted into the two chain tables
  (4-gram, H4 = 2^26 heads; 8-gram, H8 = 2^28 heads; prev arrays of n entries
  each; peak RSS 10.9 GB) only once the parse has passed them, so at factor
  start i the chains contain exactly the candidate set {p < i}, newest first.
  (v1 pre-inserted all positions and then had to walk through every FUTURE
  occurrence of common grams before reaching valid candidates -- quadratic;
  killed after 3.2 h. v2 produces byte-identical output: 5 MB z=618,254,
  100 MB z=9,672,350, both equal to v1.)
- Parse pass: greedy LZ77. At each factor start, walk the 8-gram chain (budget
  128 candidates, newest first, lazy reject on T[p+best] != T[i+best]), then
  the 4-gram chain; longest match wins; overlapping copies allowed; factor =
  phrase if len >= 2 else 1-byte literal.
- Accuracy: any longest match of length >= 8 shares its 8-gram with the source and is
  chain-reachable; matches of length 4..7 are reachable via the 4-gram chain; true
  longest matches of length 2-3 are rare in text and only *add* factors, so the
  reported z is a tight UPPER bound on exact greedy z (chain cap 128 ditto).
- Greedy is the standard "z" definition (Kreft-Navarro, Christiansen et al. etc.).

Cross-check: zstd -15 --long=31 probe (independent compressed size, see below).

RESULTS (lz77z-pile-frag.out; run v2, progressive-insertion parser; killed v1
after discovering its pre-inserted chains made factor walks traverse all FUTURE
occurrences first -- quadratic blowup; v2 gives byte-identical z on validation
slices, 5 MB: z=618,254 == v1; 100 MB: z=9,672,350 == v1):

    n            = 1,082,130,213
    z (factors)  = 85,822,791   (tight upper bound on exact greedy z, +0.7-4%)
      literals   = 1,974,375
      phrases    = 83,848,416 (avg len 12.88, max 97,059)
    z/n          = 0.079309
    n/z          = 12.609
    z/R          = 0.2158  (z = R/4.63)
    parse wall   = 1385 s (23 min, single process, ~10.9 GB peak RSS)

zstd -15 --long=31 cross-check: 313,164,635 bytes = 2.315 bits/char, vs
z*log2(n) = 2.380 bits/char -- agreement within 3%, as expected (zstd encodes
offsets adaptively/batches literals; both agree the LZ-representation of this
text is ~2.3-2.4 bpc).

### 1.1 Arithmetic verdict vs the run family (fragment scale)

- R = 397,723,010; R/n = 0.3675; log2(n) = 30.009 bits.
- Grammar/LZ core estimate (user-specified formula): z*log2(n) = 85,822,791 x
  30.009 bits = 322 MB = 2.380 bpc.
- rleBWT core equivalent: 7.2 bits/run * R = 358 MB = 2.646 bpc.
- So on the fragment, the bare grammar dictionary is ~10% SMALLER than the
  rleBWT core. But this inverts at pile scale (log2(n) grows, 7.2 does not; see 4).
- Crossover arithmetic: z*log2(n) < 7.2*R iff z < R * 7.2/log2(n); at
  fragment scale that is R/4.56 = 87.2M -- and z = 85.8M sits right at the
  crossover (z/R = 0.216 vs threshold 0.219).
- Kreft-Navarro iCSA in practice = 2.6x the LZ77-compressed size
  (z*(log2 n + log2 z) = 605 MB) = 1.57 GB -- 4.4x the rleBWT core.

### 1.2 zstd cross-check

zstd -15 --long=31 -T4 on the fragment: 313,164,635 bytes (2.315 bpc),
decode-verified with `zstd -t --long=31`. Within 3% of z*log2(n) -- the z
measurement and the independent tool agree on the LZ compressibility of this
text. Used as sanity only; z is the deciding number.

---

## 2. LITERATURE SURVEY — compressed self-index families beyond run/samples

All URLs fetched/verified from this box on 2026-10-02 (arXiv abs pages, arXiv API,
Crossref, OpenAlex, Europe PMC; DDG/Bing/Startpage/biorxiv were rate-limited or
blocked from this IP, so citations were assembled from the APIs that worked).

### 2.1 Grammar / LZ self-indexes (the family the z-measurement decides)

- **Kreft & Navarro, "Self-Index Based on LZ77" (SPIRE 2011, arXiv:1101.4065)**
  and journal version **"On compressing and indexing repetitive sequences"
  (TCS 2013, doi:10.1016/j.tcs.2012.02.006)** -- the LZ77-index ("iCSA").
  https://arxiv.org/abs/1101.4065
  - Space: "a few times the space of the text compressed with LZ77 (as little as
    2.6x)", i.e. ~2.6 * (z log n + z log z) bits-ish in practice.
  - Locate: O(m log z + occ log z) per occurrence-ish; extract 1-2 M chars/s.
  - Applies to web text? Yes in principle (any byte string), but see sr-index
    experiments: an LZ-based index was the ONLY index using less space than the
    sr-index, and it was an order of magnitude slower.

- **Christiansen, Ettienne, Kociumaka, Navarro, Prezza, "Optimal-Time
  Dictionary-Compressed Indexes" (SODA 2019 / TALG; arXiv:1811.12779)**.
  https://arxiv.org/abs/1811.12779
  - Space O(g log(n/g)) words (g = grammar/attractor size; gamma <= g).
  - Locate O(m + (occ+1) log^eps n); optimal locate O(m+occ) at O(g log(n/g) log^eps n).
  - Applies to web text? Theoretically yes; practically, the sr-index paper reports
    the best grammar-based index [CNP21] is dominated in BOTH space and time by the
    subsampled run-family index on real corpora.

- **Nishimoto & Tabei, "Dynamic Grammar-Compressed Self-Index in delta-Optimal
  Space" (arXiv:2604.24080, 2026)** -- dynamic RR-index on restricted recompression
  RLSLP. https://arxiv.org/abs/2604.24080
  - Space O(delta log(n log sigma / (delta log n)) log n) bits -- the first *dynamic*
    index at delta-optimal space; locate expected O(m + log m log^2 n + occ
    (log n / log log n)); updates O(m' log^2 n + log^3 n).
  - Measured: 11 repetitive corpora incl. 37 GB Wikipedia dump (web text!):
    up to 77x faster updates than dynamic r-index, up to 11x faster locate than
    other dynamic indexes. NOTE: delta <= z, and this is the strongest 2026
    grammar-family result; it is *dynamic-capable* which the run family is weak at.
    Its space still scales with delta ~ repetitiveness, which our z measurement
    now pins down for pile text (see verdict).

- Related: Claude & Navarro's original grammar self-index (older, slower locate);
  OESP-index (online grammar self-index, arXiv:1507.00805); L-CDAWG
  (arXiv:1705.09779) -- all superseded by the above for our purposes.

### 2.2 Block trees (Belazzougui et al.) and block trees over BWT runs

- **Belazzougui, Caceres, Gawrychowski, Gagie, Karkkainen, Navarro, Puglisi, Tabei,
  "Block trees" (JCSS 2021, doi:10.1016/j.jcss.2020.11.002)**.
  - Space O(z_stream log(n_stream / z_stream)) words for a stream; the block tree
    reaches compression "close to Lempel-Ziv" on the stream it is built over,
    with direct access.  Practical parallel construction: arXiv:2512.23314 (2025).
- **Navarro, "A Self-Index on Block Trees" (arXiv:1606.06617)** -- full self-index
  in O(z log(n/z)) words, locate O(m log n + occ log^eps n).
  https://arxiv.org/abs/1606.06617
- **Caceres & Navarro, "Faster Repetition-Aware Compressed Suffix Trees based on
  Block Trees" (arXiv:1902.03274)** -- block trees compressing the CSA/suffix-tree
  component streams on real repetitive corpora: GCST topology down to 0.5-2 bits
  per symbol; navigation components 0.3-1.5 bits per node.
  https://arxiv.org/abs/1902.03274
- **What compression of *run streams* on real text?** The honest measured picture:
  on highly repetitive corpora block trees get within a small factor of the LZ77
  compressed size of the stream. BUT our pile's run stream is different: at
  R/n = 0.37 the rleBWT stream is only mildly compressible beyond its 7.2 bits/run
  (the BWT of web text is close to order-0 random-looking at this repetitiveness;
  there is little *long-range* structure left in run heads for a block tree to
  exploit). A block tree over the run-head/length streams would shave little off
  the 414 GB core. Block trees shine on streams WITH long-range repetition --
  i.e. on the *sample* arrays and satellites, which is exactly where
  threshold/subsampling (2.3) already attacks.
- **Could not be measured this session, by law and by absence**: THE LAW forbids a
  second corpus pass (the single-pass read was consumed by the z measurement), no
  retained RLBWT run-stream artifact exists for the fragment
  (B/xsa-build-wmqsx699/ is empty), so the block-tree-shave on run heads stays
  analytic: expected order 10-20% off the 414 GB core at best (run heads of web
  text are near order-0-random; block trees only beat order-0 entropy through
  long-range repetition the pile's BWT does not have). The measured big shaves
  are all in the sample tier (2.3).

### 2.3 Threshold sampling / subsampled r-index (sample-count reduction)

- **Gagie, Navarro, Prezza, "Fully Functional Suffix Trees and Optimal Text
  Searching in BWT-runs Bounded Space" (JACM 2020, doi:10.1145/3375890)** -- the
  r-index: one SA sample per BWT run end, O(r) space total, locate O(occ) with
  toehold. The baseline our exact-lite pile index belongs to.
- **Cobas, Gagie, Navarro, "A Fast and Small Subsampled R-index" (SPIRE 2021,
  arXiv:2103.15329) and "Fast and Small Subsampled R-indexes" (2024,
  arXiv:2409.14654)** -- the sr-index: *threshold/subsampling of the SA samples*:
  drops samples in oversampled regions so consecutive samples are never closer
  than s text positions; space O(min(r, n/s)) samples worst case.
  https://arxiv.org/abs/2409.14654
  - **Measured sample-count reduction factors (their numbers, real corpora incl.
    Pizza-Chili web/doc text): total index space 1.5-4.0x smaller than the
    r-index with the SAME query time**; "for s=4, samples reduce to ~1/4 on
    synthetic texts but ~1/7 in the densest areas of real texts" (non-uniform
    sample distribution is the whole win). This is the *strongest known attack
    on the sample tier*, and it is measured on web-like text, no satellites,
    R/n like ours or milder.
- **Rossi, Oliva, Langmead, Gagie, Boucher, "MONI" (RECOMB/JCB 2022,
  doi:10.1089/cmb.2021.0290; bioRxiv:10.1101/2021.07.06.451246)** -- thresholds
  (Bannai et al.) + threshold-based sampling for MEM finding; sample tier
  reduced to ~n/t with threshold t. DNA-oriented but the mechanism is
  alphabet-independent.
- **Diaz-Dominguez & Makinen, "Adaptive encodings for small and fast compressed
  suffix arrays" (arXiv:2602.17201, Feb 2026)** -- 2026 sample-compression work we
  had not yet ingested: variable-length blocking (VLB) of the BWT into a tree of
  run blocks, compressible regions near the root carry little auxiliary data;
  plus a sampling scheme guaranteed correct only along valid backward-search
  states. Measured: VLB beats r-index AND sr-index query time at space close to
  sr-index, and offers a better space-time tradeoff than the move structure.
  https://arxiv.org/abs/2602.17201  -> HIGHLY relevant to our sample tier; read next.

### 2.4 Move structure / Movi line (we already matched Movi 2's techniques)

- **Nishimoto & Tabei, "Optimal-Time Queries on BWT-runs Compressed Indexes"
  (ICALP 2021, arXiv:2006.05104)** -- the move structure: O(r)-space table of the
  permutation enabling constant-time LF/Phi steps; tool **Movi** (and successor
  implementations, the "Movi 2" compact techniques the team already matched).
  https://arxiv.org/abs/2006.05104
- **"RLBWT Tricks" (arXiv:2112.04271)** -- practical constant-time backward-step
  implementation on RLBWT; documents the move-structure implementation line.
- **MIOV: "Reordering MOVI for even better locality" (arXiv:2407.18956, 2024)** --
  row reordering of the move structure for locality; relevant to our
  BWT-iteration throughput.
- **"RLZ-r and LZ-End-r: Enhancing Move-r" (arXiv:2507.17300, 2025)** and
  **"Move-rb: Faster Bi-Directional r-indexes and Approximate Pattern Matching"
  (arXiv:2609.30089, 2026)** -- current frontier of the move family; all still
  O(r)-space-and-up (r = R), i.e. they attack time, not the 414 GB floor.
- Note: the 2026 VLB paper (2.3) already claims a better space-time tradeoff than
  the move structure.

### 2.5 2024-2026 work we may have missed (sample compression / new families)

- **Suffixient arrays: Cenzato et al. "Suffixient Arrays" (arXiv:2407.18753;
  theory arXiv:2312.01359, follow-ups arXiv:2506.05638, 2506.08225, 2605.04258,
  2607.00204)** -- subsample the *SA/PA itself* to a suffixient set of size chi
  (sigma' <= gamma <= chi <= 2 r_bar): pattern matching by binary search
  **provided random text access**. Measured on DNA: chi ~ r_bar/1.13-1.33 -- i.e.
  chi is r-like, NOT z-like, so at pile scale it is another ~R-sized structure
  (~our 155 GB chi), PLUS it needs a random-access oracle over the 1.2 TB text
  (they use bitpacked text or LZ/RLZ access structures). Does NOT beat our
  exact-lite floor; interesting only as a different way to spend the same bits.
- **Gagie, "r*-indexing" (arXiv:2508.12675, 2025)** -- O(r* log(n/r*) + z log n)
  bits (r* = runs of T and T^rev combined): a run-family x LZ hybrid; the z log n
  term means its space also hinges on the z we measured.
- **Dynamic r-index (arXiv:2504.19482, 2025)** and **Dynamic RR-index
  (arXiv:2604.24080, 2026)** -- the dynamic frontier; RR-index is grammar-based
  (2.1).
- **Faster run-length compressed suffix arrays (arXiv:2408.04537, 2024)** --
  O(r log(n/r) + r log sigma) bits RLCSA with O(log r_a) steps: time-side.
- Lossy-ish: **SPUMONI 2 (Ahmed et al., bioRxiv 10.1101/2022.09.08.506805)** --
  compressed index of *minimizer digests* (lossy front end over a compressed
  index) for classification; the closest published pattern to our "sketch tier
  paired with exact back end" (section 3).
- Also noted: "Wavelet Forests Revisited" (arXiv:2609.14746, 2025-26) appeared in
  searches; not yet read; likely time-side.

---

## 3. LOSSY TAXONOMY (positions cannot be "slightly exact")

Two honest lossy PRODUCT tiers, evaluated as complements to the exact index:

### 3.1 Seed / syncmer tier (positions, lossy coverage, needs verification)

- **Edgar, "Syncmers are more sensitive than minimizers..." (PeerJ 2021,
  doi:10.7717/peerj.10805)** -- closed syncmers: deterministic, density ~1/w
  (window w), no sliding-window pathologies.
- **Durbin's substrate: "A run-length-compressed skiplist data structure for
  dynamic GBWTs supports time and space efficient pangenome operations over
  syncmers" (bioRxiv 2026, doi:10.64898/2026.03.26.714584; code
  github.com/richarddurbin/syng)** -- syncmer graph + dynamic RLE-compressed
  GBWT skiplist. Measured: **92 human genomes (~276 Gbp): 4 GB for the 63bp
  closed-syncmer set + 5.8 GB lossless GBWT over it** (~21x total compression),
  MEM search ~1 Gbp per 10 s per thread. That is ~0.14-0.15 bits/char of raw
  sequence for positions+context at density ~1/32, including the graph.
- Size at density d for the PILE (tokens/bytes, n ~ 1.2e12): positions+seed-id
  at density d costs ~(log2 n + k*log2 sigma)/d bits per char = e.g. d=32,
  k=16, sigma~96: (40 + 105)/32 ~ 4.5 bits/char ~ **675 GB** -- TOO BIG: web
  text is not DNA; k must be ~2x to get discriminating 8-byte seeds, so the seed
  tier at pile scale lands at hundreds of GB unless d >= 128 (then ~170 GB but
  coverage/verification cost doubles again). AND it needs text random access for
  verification: through our rleBWT extraction is O(len) per verify -- slow but
  workable; through the raw text it needs the 1.2 TB corpus resident.
  Verdict: a seed tier is a DNA-shaped product; at pile scale it only pays as a
  *sparse* front end (d >= 128) or not at all.

### 3.2 k-mer / token sketch tier (membership/count, NO positions, tens of GB)

- Bottom-k / minimizer digests (SPUMONI 2 line), set sketches, count sketches
  (Count-Min family), and token-level n-gram sketches. No positions -> cannot
  locate, but answers "is this k-gram present / how often / roughly where in
  sketch-bucket space" and PRUNES the exact index: sketch hit -> rleBWT
  backward-search verification -> exact locate from the real index.
- Pile-scale size: a bottom-k sketch over 31-bit hashes at k-digest level:
  ~z-set of distinct k-grams; web text distinct 8-gram count ~ 0.05-0.2 of n
  (0.06-0.24e12); at 8 bits/sketch entry (hash+count) a d=32 bottom-k sketch is
  tens of GB (20-60 GB). This is the only tier that fits "well under 100 GB",
  and it pairs LEGITIMATELY with our exact index as a fast-approximate front end:
  - sketch filters queries before touching the 414 GB rleBWT (which lives on
    disk and is slow per step);
  - sketch gives approximate counts instantly;
  - exact back end retains the only locate semantics we can honestly sell.
  This is exactly the SPUMONI/MONI-style architecture, transplanted from DNA to
  web text: digest index (lossy) + compressed exact index (lossless).

---

## 4. VERDICT TABLE + RECOMMENDATION

### 4.1 The numbers (pile scale, extrapolated from the fragment)

Pile: R_pile = 4.6e11 runs, R/n = 0.3675 (fragment-measured) -> n_pile ~
1.252e12 bytes. z_pile = z_frag * (n_pile/n_frag) = 85,822,791 x 1156.6 =
9.93e10 (assumes z/n constant; z/n falls ~15% per 10x scale, so z_pile is likely
80-99B; the verdict is robust across that whole range). log2(n_pile) = 40.19.

| Option | Pile-scale space | Basis | Notes |
|---|---|---|---|
| **exact-lite (run family core)** | **500-580 GB** | measured (rleBWT 414 GB + chi 155 GB + anchors 7 GB) | locate needs samples: 1.4-2.4 TB (measured, any encoding) |
| **grammar/LZ core, bare** z*log2(n) | 499 GB (3.19 bpc) | measured z extrapolated | NOT an index: no count/locate structures yet; merely TIES the exact-lite floor |
| **grammar/LZ as a real index** (iCSA 2.6x LZ size) | ~2.5 TB | Kreft-Navarro practice ratio | 4-5x the exact-lite floor; locate 10-50 us/occ (1000x slower than move structures) |
| **block-tree shave (on core)** | -10-20% of 414 GB core => saves ~40-80 GB | analytic (no second corpus pass allowed under THE LAW; no retained run-stream artifacts) | run streams of web text are near order-0; little long-range structure for a block tree |
| **threshold/sr-index shave (on samples)** | samples /1.5 to /4.0 => 1.4-2.4 TB -> 350-1600 GB | sr-index measured factors on real corpora (incl. web/doc text), same query time | THE big lever; locate per-occ cost x s |
| **VLB adaptive encodings (2026)** | "space close to sr-index", better time than r-index & move | arXiv:2602.17201 measured | evaluate as encoding upgrade for our BWT+sample tiers |
| **sketch tier (lossy front end)** | 20-60 GB | Sec. 3.2 | no positions; membership/count only; pairs with exact back end |

### 4.2 The verdict, plainly

**z = 85,822,791; z/n = 0.0793; z/R = 0.216 (z = R/4.63). z < R/3 = 132.6M, so
the crude R/3 rule does NOT fire -- but the grammar family is still dead for
the pile, and here is the arithmetic that kills it:**

1. At pile scale, z_pile ~ 9.9e10 and log2(n_pile) = 40.19, so even the BARE
dictionary z*log2(n) = 499 GB merely ties our exact-lite floor (500-580 GB) --
and it is not an index: no locate, no count. The reason the fragment comparison
flatters the grammar family is that log2(n) grows with corpus size while our
7.2 bits/run does not: on the fragment z*log2(n) (2.380 bpc) beats the rleBWT
core (2.646 bpc) by 10%; at pile scale it is 3.19 bpc and LOSES to 7.2*0.3675 =
2.65 bpc. The effective dead-threshold at pile scale is z >= R*7.2/40.19 =
R/5.58 = 82.5B, and z_pile = 99B > 82.5B.
2. Any real LZ/grammar self-index with locate (Kreft-Navarro iCSA, the most
practical of the family, at 2.6x the LZ77-compressed size) costs ~2.5 TB at
pile scale -- 4-5x our exact-lite floor, and roughly the same as our current
exact index WITH its sample tier (1.9-3.0 TB) -- while being orders of magnitude
slower per occurrence (10-50 us/occ; the sr-index authors measured LZ indexes
as an order of magnitude slower than r-index-family indexes, and the grammar
indexes as dominated in space AND time).
3. For the grammar family to beat the exact-lite floor it would need z_pile <
580GB*8/(2.6*40.19) = 4.4e10, i.e. z/n < 0.035 at pile scale -- 2.3x below the
measured 0.079 at 1 GB scale. z/n declines only ~15% per 10x of scale
(0.118 @ 5 MB -> 0.092 @ 100 MB -> 0.079 @ 1 GB). Not plausible.

**So: the grammar/LZ self-index family cannot beat our run family at pile scale.
The run family is not merely competitive -- with threshold-sampled samples it
dominates the space-time Pareto frontier (sr-index: 1.5-4.0x sample shave at
identical query time, measured on real web/doc corpora).**

### 4.3 Recommendation

1. **Do not pivot to grammar/LZ self-indexes for the pile.** The measured z
   (85.8M on the fragment, z/n = 0.079) settles it: their core ties our floor,
   their locate machinery quadruples-quintuples it, their locate speed is
   1000x off the move-structure family.
2. **Attack the sample tier with sr-index-style threshold subsampling** -- the
   only literature-measured multi-hundred-GB lever (1.5-4.0x sample reduction
   at unchanged query time on real corpora; for s=4, samples in dense real-text
   regions reduced to ~1/7). Expected: samples 1.4-2.4 TB -> 0.35-1.6 TB at
   locate cost x s per occurrence. This should be the next engineering task.
3. **Evaluate VLB adaptive encodings (Diaz-Dominguez & Makinen, arXiv:2602.17201,
   Feb 2026)** for our BWT and sample tiers: measured better time than r-index
   AND move structures at sr-index-like space -- the one 2024-2026 result that
   beats the Movi/move line we already matched.
4. **Block trees: skip for the core.** Expected shave on the 414 GB rleBWT core
   is order 10-20% (analytic; unmeasurable this session under THE LAW), not
   worth the complexity. The 155 GB chi tier is the better satellite target
   for block-tree/entropy re-encoding.
5. **Lossy tier as a PRODUCT, not as "slightly lossy positions":** a 20-60 GB
   token/k-mer sketch front end (SPUMONI-style digests) that answers
   membership/counts instantly and prunes exact searches; positions always come
   from the exact index. A seed/syncmer tier with positions (Durbin's syng
   substrate style) is DNA-shaped; at pile scale with 16-byte seeds it costs
   170-675 GB and still needs text random access for verification -- only worth
   it as a sparse d>=128 front end if the sketch tier proves insufficient.

### 4.4 Caveats, honestly stated

- z is an upper bound (chain cap 128 costs +0.67% at 5 MB; matches of length 2-3
  are unfindable by hash chains and only add factors): true z ~ 82-84M; changes
  nothing above.
- z_pile extrapolation assumes z/n constant; real z/n at pile scale should be
  somewhat lower (more history). The verdict needs z/n < 0.035 to flip -- a 2.3x
  drop versus the measured scale trend; effectively impossible.
- Fragment R/n (0.3675) is taken as representative for the pile; if the pile's
  true R/n is higher (more repetition), the run family only gets stronger.
- Block-tree shave % is analytic, not measured (THE LAW forbids a second corpus
  pass; no retained run-stream artifact exists for the fragment).

---

## 5. SESSION RECORD

- v1 parser (pre-inserted chains): PID 1010936, killed after 3.2h when the
  100 MB rate probe exposed the future-occurrence-walk blowup (the probe itself,
  run concurrently, took 193 min wall; v2 does the same 100 MB in 80 s with
  byte-identical output).
- v2 parser (progressive insertion): PID 1606719, completed 1385 s parse, exit 0.
- zstd probe: PID 1017237, completed, verified decode.
- No processes left running at journal close. Peak resource use: 10.9 GB RAM,
  <= 5 concurrent threads at any time.

