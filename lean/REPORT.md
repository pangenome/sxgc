# Math lane report (laneW, GLM worker)

Session scope: three items on the suffixient-array theory. All work in `lean/`.
`lake build` green; every claimed theorem is kernel-checked with
`#print axioms` ⊆ {propext, Classical.choice, Quot.sound} (house rules: no
sorry-stuffing, no native_decide). `Sxgc.lean` is byte-untouched: its four
pre-existing proof holes are unchanged. Existing evals (O2Eval and the rest of
the default target) compile unchanged.

## Item 1 — O2_bounded: PROVEN OUTRIGHT

The statement-locked bounded successor `SxgcBounds.O2_bounded` (the open
proposition from the laneR handoff, statement unmodified) is now a theorem.

Route — a strictly stronger stream theorem, proven first:

- `scanAux_nodup` (`lean/SxgcNodup.lean`): for ANY triple stream `ts`
  (not just `triplesOf` a text):
  `(ts.map Triple.sa).Nodup → (∀t∈ts, t.sa < N) → (∀t∈ts, (t.lcp : Int) ≤ MAXINT) → (scan N ts).Nodup`
  The invariant is ten hypotheses threaded through `evalStep`/`upd`:
  out-Nodup, armed-not-in-out, armed pairwise distinct, armed not in future
  positions, out not in future, `(N − pSa) ∉ out`, only-p holds `(N − pSa)`,
  armed-p-with-`(N − pSa)` forces `m = MAXINT` (boundedness is used exactly
  once: it blocks re-arm of a just-emitted position at run-length-1
  boundaries — this is where unbounded streams duplicate).
- `scan_nodup`: top-level from the `scanAux N rest t.c t.sa MAXINT defaultR []`
  initial configuration (defaultR's inactivity makes the R-hypotheses vacuous).
- `triplesOf_sa_nodup`: the sa values of `triplesOf T` are exactly the
  entries of `saOrder (T.reverse ++ [0])` (List.range/getElem! bridge),
  which is `Nodup` (`saOrder_nodup`, already proven).
- `O2_bounded_true : SxgcBounds.O2_bounded`: assemble via
  `triplesOf_streamGood` (sa < N) and the Nat→Int coercion of the
  boundedness hypothesis.

Notes:
- The `positive T` hypothesis of the locked statement is NOT used by the
  underlying argument — the stream theorem is strictly stronger — but the
  locked statement is what is proven, byte for byte.
- The stale handoff note in `SxgcBounds.lean` ("No proof of O2_bounded is
  supplied; finite testing is its statement warrant") should be updated by
  the measurement owner; per statement-lock discipline this file was not
  modified.
- Empirical warrant retained from before the proof: 1,474,560
  distinct-sa bounded streams (length ≤ 4), zero Nodup violations.

## Item 2 — seam repair: identity half PROVEN, reordering half statement-locked

`lean/SxgcSeam.lean` formalizes
`bit6/sxi_logs/sep-convention/repair2/DERIVATION.md` at the frame level:

- Model: `suffixAt T i` = finite suffix at 0-based `i` (the padded frame's
  row content after removing the dollar rows); `rotAt T i` = cyclic rotation
  at `i` (the canonical 0x1E corpus order); `lexLt` = first-difference
  lexicographic order.
- `firstDiff`: two prefix-incomparable lists have a first-difference
  decomposition (common prefix, distinct bytes, remainders).
- `rot_agree`: **order agreement on prefix-incomparable rows** — the
  mismatch that decides the suffix comparison decides the rotation
  comparison identically. This is the load-bearing lemma.
- `seamPos`: a row is a seam row iff its suffix is prefix-comparable with
  some other suffix — exactly the rows the repair may touch.
- `identity_outside_classes`: **for two non-seam rows, finite suffix order
  and cyclic rotation order agree** (DERIVATION: "every comparison outside
  the union mismatches before either suffix ends and retains its order").
- `agreement_with_nonseam`: **a non-seam row is order-stable against every
  row, seam or not** — no in-class reordering can displace a row across a
  non-seam row (DERIVATION: "every rotation in a class still starts with
  that class's prefix; therefore it cannot move outside the class").
- `lexLt_total_same_length`, `rotAt_length`, `rotation_cmp_total`: the
  cyclic comparison (`cycCmp`: rotation order, complete-rotation ties by
  increasing position) is total on distinct positions.

Statement-locked open:
- `seamRepair_reorders_in_classes`: sorting a seam class by exact cyclic
  LCE (ties by increasing position) equals the restriction of the cyclic
  rotation order to the class. Proven core: totality + no-crossing. Open
  parts: the class-partition structure (terminal-suffix prefix intervals
  nested-or-disjoint, maximal-union partition) and the equivalence of the
  r-space spliced output (Phi anchors, run splicing, coalescing) with this
  order. Measured warrant: yeast235, seven maximal classes, 13,503
  candidate rows, repaired output byte-identical to the dense independent
  cyclic oracle.

## Item 3 — boundary-delta identity: statement-locked, obstruction recorded

- `insertSep T p` inserts the reserved separator (0x1E = 30) at `p`;
  `boundaryDelta_bounded` states: ∃ C, ∀ T ps, chi of the k-fold
  insertion and chi of T differ by at most C·k.
- Measured instances: yeast k=1 (single seam class, empty repair);
  yeast235 k=9,901: 85,404,240 → 85,404,336 (+96).

Obstruction to deriving it from `chi_eq_maxClasses` (honest report, not a
failed attempt hidden): the characterization counts *distinct inclusion-
maximal coverage classes*, and separator insertion perturbs coverage sets
in two coupled ways. (a) Position shift: every position after a separator
moves, so coverage sets are not literally comparable; one needs a
transport argument between position spaces. (b) Class spawning: each
separator boundary can split an existing maximal class and can create new
ones (the separator byte's own coverage), and the *interaction* between
adjacent boundaries is not obviously bounded per-boundary — an explicit
constant C would have to charge both directions (chi can grow AND shrink:
measured +96 at k=9,901 is a net of splits/merges). The scan-level
mechanism (Item 1's theorem) constrains χ only through Nodup of the
emission stream, which does not transfer to a count comparison. What a
proof needs: a coverage-class transport lemma through single-separator
insertion (the k=1 case), then induction on separators with the
observation that separators are pairwise non-adjacent corpus bytes. The
k=1 case alone is a real theorem (the seam analysis of Item 2 is the
cyclic-order side of it; the coverage-class side is untouched).

## Files

- `lean/SxgcNodup.lean` (new): Item 1 (stream theorem → O2_bounded).
- `lean/SxgcSeam.lean` (new): Item 2 (identity half + totality),
  statement-locks for Items 2/3.
- `lean/lakefile.lean`: both new libs registered.

Axiom audit (all ⊆ {propext, Classical.choice, Quot.sound}):
O2_bounded_true, scanAux_nodup, scan_nodup, triplesOf_sa_nodup,
identity_outside_classes, agreement_with_nonseam, rot_agree,
lexLt_total_same_length, rotation_cmp_total, firstDiff, lexLt_shape.

No commits made (agents never commit). `Sxgc.lean` and `SxgcBounds.lean`
statements untouched.

## Cover-cases completion lane (this session): chi_fam_bounds COMPLETE, floor program fully composed

Task: close the two marked cover stubs (`fam_run_occ_le`, `fam_cover`) and
assemble the floor composition. Result: everything kernel-checked; the lane
went beyond the two stubs and closed the entire family-specific floor program.

### Mandatory targets — DONE, zero unmarked sorries
- **`fam_run_occ_le` PROVEN**: an occurring pure run `x_i^{m+1}` fits in the
  right run (`m+1 ≤ L − p_i`). Route: every window letter is `x_i`, so every
  window position is in block `i` off the marker (`fam_run_iff`); block-relative
  offsets are consecutive (`div_add_mod`); a consecutive offset window avoiding
  `p_i` lies wholly left (then `m+1 ≤ p ≤ L−p` by the half grid) or wholly right
  (then it fits before the block end). Boundedness used nowhere — the lemma is
  pure block arithmetic.
- **`fam_cover` PROVEN** (statement-locked, unmodified): every requirement is
  covered by one of the `3k−1` positions. Case analysis: `w = ε` → cover the
  letter at its own occurrence (marker → marker-successor; run letter →
  block-end, using `L ≠ p_j` from the half grid); `w = x_i^m` with the window
  `w++[c]` at `s` → `c` marker (either block: cover at `s+m+1`, family 1 via
  `fam_mark_iff` at the last window position) or `c` run letter: same block
  (run extension: `fam_run_occ_le` + reconstructed window at the block end,
  family 2) or different block (the position before the change is in block `i`,
  so `s+m = (i+1)(L+1)` exactly, cover at `s+m+1`, family 3). All covers via
  `coversAt_of_window`/`coversAt_single` — never a walk, never an enumeration.
- **`chi_fam_upper`** (`χ ≤ 3k−1` via the explicit `famV` and
  `chi_le_of_suffixient_mem`) and **`chi_fam_bounds`**: `2k ≤ χ(famText k L ps)
  ≤ 3k−1` on the half grid — the battery's empirical law, now a theorem.

### Composition — DONE (beyond the two stubs)
- **`fam_forced_incompat` PROVEN** (was a skeleton stub): distinct half-grid
  assignments admit no common correct locate-one oracle. New machinery:
  `coversAt_letter_iff` (single-letter cover ⟺ prefix ends with it, via
  `List.getElem_of_eq`), `fam_marker_cover_unique` (the marker query's cover is
  uniquely the marker-successor position).
- **`floor_theorem_shape` PROVEN** (statement-locked, unmodified): from the
  counting bound `(L/2+1)^k ≤ 2^(s+1)−1`, `2k ≤ χk ≤ 3k−1`, `n = k(L+1)`:
  `χk · log2(n/χk) / 3 ≤ s+1`. The Ω(χ·log(n/χ)) arithmetic core of the floor.
  Route: `2^(k·log2 m) ≤ m^k < 2^(s+1)` gives `k·log2 m ≤ s`; `χ ≥ 2k` cancels
  the `k` in `n/χk ≤ L/2+1`; monotone log2; assemble and divide by 3.
  Toolchain notes: core has NO `Nat.log2_spec`/`log2_le_log2`/`log2_mono` —
  built from `Nat.le_log2` as `pow_log2_le`/`log2_mono`; NO Mathlib `ring`/
  `set`/`by_contra` — replaced with core equivalents throughout.

### The witness: skeleton FALSE as stated — true form PROVEN
- `fam_oracle_witness` as originally locked is **false** (for
  `2^(s+1)−1 ≥ (L/2+1)^k` a decoder can index one correct answer per member;
  pairwise-distinct answers fit). Documented in place; statement left with
  its single sorry per statement-lock discipline (not silently edited).
- **`fam_oracle_witness_size` PROVEN** (the honest replacement, with the size
  flip `2^(s+1)−1 < (L/2+1)^k`): NO `s`-bit index scheme under one fixed
  decoder answers locate-one correctly on every half-grid member. Route:
  `family_counting` applied to the family — but the product-list bookkeeping
  (size + Nodup) is discharged by a **digit encoding**: `decodeP m k z` (the
  base-`m` digits of `z`) turns the family into `(range (m^k)).map ...`, so
  Nodup is `List.nodup_range` + injectivity (`famText_inj_markers`: equal
  texts pin equal marker offsets block-by-block; `digits_inj`: equal digits
  force equal numbers below `m^k`). No product-list machinery anywhere.

### Program state after this lane
The family-specific floor theorem is now FULLY kernel-checked end to end:
`family_counting` (pigeonhole) + `fam_oracle_witness_size` (counting on the
family) + `chi_fam_bounds` (2k ≤ χ ≤ 3k−1) + `fam_half_grid_size` (n) +
`floor_theorem_shape` (arithmetic) ⇒ `s ≥ Ω(χ·log(n/χ))` on the half-grid
family — the first space lower bound of this shape for the locate-one class.
Lean status: `lake build` green; `Main.lean` BIT 1B gate GREEN; axioms of every
flagship ⊆ {propext, Classical.choice, Quot.sound}; no native_decide; sorry
ledger: LowerBound 1 (the documented-false skeleton, unchanged), flagship
files byte-untouched.
