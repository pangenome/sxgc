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
