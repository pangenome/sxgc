# O1 outright attempt — obstruction report (2026-09-27 lane)

Target: (O1) domination — the covering obligation of `covering_given_stream`:

    ∀ x ∈ positionsT T, ∃ y ∈ scan (T.length+1) (triplesOf T), ScopeLe T x y

**Result: exact reduction proven, outright proof obstructed at a single
named scan-side statement.** All theorems below are kernel-checked
(`lake build` green, axioms propext/Classical.choice/Quot.sound only; the
sorry ledger is unchanged — no new sorries, no sorry-stuffing, and the
O1 statement itself is untouched).

## What is now PROVEN (new, this lane — appended to `Sxgc.lean`)

1. `covSet_ne_of_mem_positionsT` — every text position covers at least the
   requirement `([], c)` for its own last character.  This discharges the
   `covSet ≠ []` side condition of `exists_max_above` *unconditionally for
   text positions*.
2. `domination_of_maxHit` + `O1_iff_maxHit` — **O1 is EQUIVALENT** (positive
   texts) to the single statement `O1_maxHit`: every inclusion-maximal
   coverage class contains an emitted position.  The reduction is pure
   coverage-set algebra: every position is dominated by a maximal one
   (`exists_max_above`), and domination composes (`ScopeLe_trans`).
3. `covering_given_stream_of_maxHit` — `O1_maxHit` alone completes the
   covering half of `minimality` (via the proven `covering_of_domination`).
4. `maxHit_iff_reps` — `O1_maxHit` is equivalently "every representative
   (`reps T`) is dominated by an emitted position": the scan obligation now
   talks directly to the semantic class machinery (`reps_suffixient`,
   `count_le_of_disjoint_witnesses`).
5. `maxHit_in_class` — a maximal position dominated by an emitted position
   is in the SAME class (the hit is never "merely covering"): `O1_maxHit` is
   exactly "the scan hits every maximal class".
6. Run-edge factorization, both directions:
   * `runEdgeHit_of_maxHit` — MaxHit **implies** `RunEdgeHit` (every maximal
     class contains a run-edge position).  Necessary because every emitted
     witness is a run-edge row (`scan_emits_run_edges`).
   * `maxHit_of_runEdge` — `RunEdgeHit` ∧ `RunEdgeDominate` (every run-edge
     position is dominated by an emitted position) **imply** MaxHit.
   Together with (2): **O1 ⟺ RunEdgeHit ∧ RunEdgeDominate** (positive texts).

## The exact missing fact

**`RunEdgeHit` / `RunEdgeDominate` (equivalently `O1_maxHit`) are not
proven.** Both are statements about *which* run-edge rows the scan's
per-character LCP-window machinery selects, related to the maximal classes
of the coverage order.  The obstruction is precisely the semantic identity
of the emitted set — the project's named "Lemma 34 core":

* the scan stores per-char candidates at run-edge rows and emits on
  `psv/nsv` window closure (`fmStep`: store at run heads, emit when
  `cand.saPos ≤ psv i` and `cand.nsv < i`);
* the analogous FM-side statement was measured FALSE at scale
  (`chi_from_events_REFUTED_AT_SCALE`: the v3 "events" model undercounts on
  duplicates-600k because a true interposer row is an *interior* row of a
  size-3 cell) — the correct restricted model ("exact range-min semantics /
  endpoint rule") is still pending measurement;
* therefore no crisp invariant "emitted = the LCP-maximal witnesses of
  (char, interval)" exists yet to induct over.  Proving `RunEdgeDominate`
  first requires *settling that semantic characterization*, which is a
  measurement/design task (the endpoint-rule model), not a proof task.

## Ranked attack strategies

1. **Prove `RunEdgeHit` semantically first (no machine internals).**
   Statement: every maximal coverage class contains a position whose
   reverse-SA row is a run edge.  Evidence: 0 counterexamples on the full
   in-file battery (binary 729, ternary 243, structured probes — see evals
   beside the defs).  Attack: fix a maximal class C, let x be the member of
   C whose row is FIRST in SA(R) order; show its row cannot be interior to a
   run, using: adjacent rows share the LCP as common right-context length
   (`take_eq_iff_le_lcpOf` + the reverse-text alignment BWT[k] = char at
   position `N − sa(k)`, lcp(k) = common suffix of the two prefixes ending
   at the adjacent positions).  An interior position with the same
   char-at-position and the adjacency of reversed contexts should force a
   *strictly larger* covSet for a class member, contradicting maximality.
   Requires: the row/position alignment lemmas (BWT-char and LCP semantics
   over `triplesOf`) — currently only the pieces (`saOrder_sorted_getElem`,
   `take_eq_iff_le_lcpOf`, `triplesOf_streamGood`) exist; the composite
   alignment theorems are not yet written.  This is the cheapest first
   target and is *independent of the emission rule*.
2. **Settle the endpoint-rule emission model, then prove `RunEdgeDominate`.**
   Measurement task (model v4 = restricted-FM with exact range-minima over
   cells), then a two-machine simulation proof against `scanAux`
   (the `FmJoint`-style induction pattern is proven infrastructure:
   `fm_equivalence_bounded` shows the method scales).  With the correct
   model, `RunEdgeDominate` should reduce to: for a run-edge row k, the
   per-char candidate table at the closing window holds a position in k's
   class.
3. **Endgame composition (already reduced).**  With 1 and 2:
   `maxHit_of_runEdge` + `covering_given_stream_of_maxHit` close
   `covering_given_stream`; with the O2–O4 side (minimality lower half)
   that closes `minimality` outright.

## Honest ledger position

O1 as stated is NOT proven today.  What changed: the obligation is now a
single named class-hitting statement (`O1_maxHit`), equivalent to O1, with
a necessary-and-sufficient factorization through run edges, and with the
semantic half (RunEdgeHit) cleanly separated from the machine half
(RunEdgeDominate, blocked on the endpoint-rule model).  All reductions are
kernel-checked; all residual statements are statement-locked with in-file
differential evidence (0 counterexamples on every battery).
