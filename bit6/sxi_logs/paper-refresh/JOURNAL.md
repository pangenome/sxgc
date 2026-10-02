# paper-refresh (2026-10-02): banked results inserted into paper/main.tex

Task: insert the banked results into paper/main.tex with EXACT numbers, no new
claims. All numbers below verified against RESEARCH.md (entries from
'THE 466 FROM-ZERO MILESTONE' onward) and against the Lean sources in lean/
before insertion.

## Edits made (paper/main.tex only; nothing staged)

1. **466 from-zero, unqualified.**
   - Results: new paragraph "HPRC 466, from zero, twice." with n=1,403,221,068,491
     (1.40 Tbp), 38,790 strings, R=2,739,737,289, one knob-free command
     (xsa build --agc HPRC_r2_assemblies_0.6.1.agc --threads 48
     --verify-text-sample 10000), 27.15 h wall, chi=2,250,211,129 (\hprcchi),
     chi/n=0.1604%, chi/R=0.8214, container 52,175,300,288 bytes, sha256
     2aca539e3747414e447372b9554f14613335162fd07c00c03599a1948198a8fb,
     byte-identical to the independent validation build (second full
     from-zero construction, same hash, same size).
   - Table caption: removed the stale "placeholder pending the running build"
     qualifier (and the 2,249,968,075 BCR-frame note); now states every row is
     a from-zero build and the 466 value was reproduced by a second
     independent from-zero construction.
   - Abstract: 466 headline now says "reproduced by a second independent
     from-zero construction whose container is byte-identical".
   - Corpus size corrected everywhere: 1.44 Tbp -> 1.40 Tbp (4 occurrences:
     abstract x2, intro x2; RESEARCH.md: N=1,403,221,068,491 = 1.40 Tbp).

2. **Construction multiplicity.** New Results paragraph "Five constructions,
   one answer": pile-frag (n=1,082,130,213, R=397,723,010), chi=306,164,765
   from FIVE constructions (PFP monolithic, PFP regenerated, BCR differential,
   serial batch-merge, parallel merge tree), four output files of each merged
   route byte-identical to the monolithic reference. Honest pricing included:
   monolith 4,789 s vs chunk+batch-BCR 20,868 s; tree 19,597 s vs serial
   20,599 s (1.05x), top level 16,063 s of 19,597 s.

3. **Theory additions (Lean-cited, \leanpoint pointers at current line numbers).**
   - Covering closed: ledger in §construction rewritten. (iv) discharged
     outright via runEdgeDominate_true (SxgcRunEdge.lean:240) + runEdgeHit_true
     (SxgcRunEdge.lean:60) -> maxHit_of_runEdge (Sxgc.lean:6007) ->
     covering_given_stream (Sxgc.lean:7554). (ii),(iii) reframed as the
     remaining count-side obligations via minimality_lower_of_scan_classes
     (Sxgc.lean:1955; |S|<=chi, Lemma 34 tie-breaking, requires LCP-maxima
     characterization; 0 counterexamples on 729 exhaustive + 320
     random/binary/repetitive battery texts). Abstract + contribution item 2 +
     Open Problem 1 updated consistently (RunEdgeDominate resolved; remaining
     open obligation is the minimality lower bound). Theorem statements NOT
     touched (thm:capstone remains conditional as printed).
   - Witness revival + chi-count demotion: new §formal paragraph "Dynamics
     under append" (SxgcRevival.lean:104 isMax_J_two, :171 chi_strictly_grows
     [chi(A)=3, chi(A++B)=5, A=[1,2,2,SEP], B=[1,SEP]]; SxgcChiMono.lean:33
     chi_append_decreases, :236 chi_single_symbol_document_decreases).
   - Unique-delimiter corollary: chi_append_unique_last (SxgcUniDelim.lean:151)
     in the same paragraph.
   - Criterion C bridge (a): new §phi paragraph "Criterion C" describing the
     hybrid safety test, with bridge (a) proven (SxgcPhi.lean:570
     byteFreqGcd_one_rot_injective; Lyndon-Schutzenberger route) and bridge (b)
     (order preservation) still statement-locked; companions
     gcdOne_not_isPower (SxgcPhi.lean:296) and criterion_singleton_branch
     (SxgcPhi.lean:310) noted.

4. **Container results, honest.** New Results paragraphs:
   - "Container format v5": implicit-exception phi encoding, all gates green
     at three scales; pile-frag 2,606,290,008 bytes (2.61 GB, 0.502x, phi
     42.25 bits/run from 63.36) with warm HTTP FASTER (0.325 vs 0.362 ms);
     yeast 0.924 GB (0.653x, phi 59.19), warm 440 vs 382 ms (+15%); k10
     20.22 GB (0.713x, phi 71.96), warm 4.61 vs 3.47 ms (+33%); 29-50%
     smaller, honest latency trade.
   - "The association floor, honestly": ~42 bits/run floor; the 25-30
     preflight assumed a compressible run permutation - refuted (RESEARCH.md
     correction #19).
   - "Witness-anchored locate": mems --first, 2-bit/run sidecar (25.2 MB
     yeast / 99.4 MB frag, ~2% of artifact), 1000/1000 correct decisions,
     872/872 and 778/778 positions text-verified, yeast 0.513 ms vs 5.671 ms
     full locate (11x), frag 0.369 vs 0.410 ms, plus the honest
     interval-witness limitation (390/870 yeast, 425/778 frag misses;
     toehold fallback covers every miss).

5. **Negative results paragraph** ("Negative results, banked"): (1) stable
   interleave merges impossible (7/20 synthetic aligned pairs obstruct;
   cross-LCP recompute required); (2) witness-restricted run structures break
   backward search (witness-free runs 18.4-25.6% of r; 99-100% of 1001 real
   MEM queries/corpus hit witness-free intervals; packed-LF repair pushes the
   pile projection to 5.861 TB vs 5.378 TB baseline); (3) the run permutation
   is incompressible (gamma-Golomb = flat: yeast 28.16, pile-frag 30.35 bits;
   12.29-bit delta-entropy was a sampling artifact; >2x compression retracted).

## Validation

- pdflatex/tectonic NOT available in this environment (checked: no tex
  binary on PATH, kpsewhich absent). Syntax-check performed instead:
  brace balance 0, all \begin/\end environments matched, $ parity OK, no
  undefined \ref/\label, 49 \leanpoint macros well-formed. Multi-line \verb
  hazard introduced then fixed (verb args now single-line).
- grep: no leftover "1.44", "sec:status" (dangling ref removed),
  "placeholder", "validation-only" in paper/main.tex.
- Every inserted number cross-checked against RESEARCH.md (milestone entry,
  chunk-merge-v3, merge-tree, v6-3/v5, #19, v7, witness-anchored-locate,
  interleave-obstruction entries) and against the Lean statements cited
  (line numbers from the current working tree).

## Residual risks / follow-ups (NOT done, deliberately out of scope)

- \sxgchash still pins commit 101403519196e5129ce113eae57123227219f59e; the
  new/updated \leanpoint targets (SxgcChiMono/SxgcRevival/SxgcUniDelim,
  current SxgcRunEdge/Sxgc lines) do not exist at that commit. The hash is
  documented in the paper as updated at release time; a release-time sweep
  must bump the hash and re-verify ALL leanpoint line numbers (pre-existing
  ones may also be stale relative to HEAD).
- paper/FACTS.md still contains the stale HPRC "PLACEHOLDER - run in flight"
  row and does not cover the new banked results; the appendix points readers
  at it. Needs its own refresh pass.
- The pile-frag corpus is not in the Results table (string count not banked
  in RESEARCH.md; the 16 chunks are construction artifacts, not corpus
  strings).
- Full pdflatex compile not run here (no TeX in environment).

## Addendum (post-review of own diff)

- Removed the loose "= flat log2(r)" equation from the negative-results
  paragraph (RESEARCH.md states the numbers as flat-code cost; the exact
  log2(r) values are 26.6/28.6 vs the measured 28.16/30.35, so the paper now
  states "flat-code cost" with the measured numbers only, no equation).
- Status: edits complete; paper/main.tex modified, journal written; nothing
  staged; no live long-run PIDs owned by this lane (read-only paper edit).
