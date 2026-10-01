# Fleet round 3 — unique-delimiter monotonicity corollary (`lean/SxgcUniDelim.lean`)

Lane: `FLEET ROUND 3 - UNIQUE-DELIMITER MONOTONICITY COROLLARY`. Worktree
`pi-worktree-9454d632-...`. Date: 2026-10-03.

## Verdict

**The user's observation is now a theorem.** The full statement was proved — not just the
single-symbol-`B` fallback:

```lean
theorem chi_append_unique_last (A B : Text) (pre : Text) (d : Nat)
    (hApos : positive A = true) (hBpos : positive B = true)
    (hA : A = pre ++ [d]) (hd : d ∉ pre) :
    chi A ≤ chi (A ++ B)
```

i.e. appending *anything* (no condition on `B` beyond positivity) cannot decrease χ when the
last symbol `d` of `A` is unique in `A` (`A = pre ++ [d]`, `d ∉ pre`). This is exactly the
oracle hammer's condition `p1` (`FLEET_CHIMONO_ORACLE_REPORT.md`), hardened there over 3.0M
randomized pairs, and the positive counterpart of `SxgcChiMono.chi_append_not_monotone`.

Delivered (all in `lean/SxgcUniDelim.lean`, additions only):

| theorem | statement |
| --- | --- |
| `not_mem_left_of_unique_last` | in `A = pre ++ [d]`, `d ∉ pre`: any decomposition `A = q ++ w` with `w ≠ []` has `d ∉ q` (the unique `d` is the final symbol) |
| `rightExts_eq_nil_of_suffix_unique_last` | a nonempty suffix of such an `A` has **no right extension** |
| `requirements_subset_append_of_unique_last` | `requirements A ⊆ requirements (A ++ B)` for **every** `B` (no positivity needed) |
| `chi_append_unique_last` | the χ-count corollary on positive texts |
| `chi_append_unique_symbol` | user-facing form: it suffices that `A.getLast? = some d` and `A.count d = 1` |
| `chi_append_single_document` | document-contract corollary: appending to a one-document text (`SEP ∉ pre`) is safe |
| `rightMaximal_cons` | unfolding lemma for nonempty contexts (plumbing) |

## Mechanism, and why it is the exact negation of the demotion mechanism

`SxgcChiMono` shows χ falls when append **erases a terminal-context requirement**: a context
that was right-maximal only because it was a suffix of `A` loses that excuse, demoting an old
maximal class with no compensating creation. The unique-trailing-symbol hypothesis kills that
mechanism at the root:

* a requirement `(w, c)` with `w = []` is right-maximal in every text and keeps its right
  extension `c`;
* let `(w, c)` be a requirement with `w ≠ []`. If `w` were right-maximal in `A` *via the
  suffix branch*, then `w` is a nonempty suffix of `A`, hence ends in the unique symbol `d`;
  but `d` occurs only at the very last position of `A`, so `w` has **no** occurrence with a
  symbol after it, i.e. `rightExts w A = []` — contradicting `c ∈ rightExts w A`. Therefore
  `w` is right-maximal in `A` through **two distinct right extensions**, and right extensions
  only accumulate under append (`rightExts_append_subset`), so `w` stays right-maximal in
  `A ++ B` and `c` stays a right extension. ∎

So the safe condition is not a bound on the appended material at all: the *delimiter alone*
blocks terminal-context erasure.

## Proof shape (index-level plumbing isolated in two helpers)

1. `not_mem_left_of_unique_last`: from `A = q ++ w` with `w ≠ []`, `|q| ≤ |pre|`, so
   `q = A.take |q| = (pre ++ [d]).take |q| = pre.take |q|`; thus `d ∈ q ⟹ d ∈ pre`. Clean,
   no `getElem` arithmetic.
2. `rightExts_eq_nil_of_suffix_unique_last`: unfold `rightMaximal.isSuffix` to get
   `w = A.drop (|A| - |w|)`; with `|w| ≥ 1` and `|A| = |pre| + 1` this gives
   `w = pre.drop (|A| - |w|) ++ [d]` (`List.drop_append_of_le_length`), so `d ∈ w`. An
   occurrence of `w ++ [c]` in `A` (`occurs_eq_true`) yields a decomposition
   `A = (A.take i ++ w) ++ ([c] ++ rest)` with `d ∈ A.take i ++ w`, contradicting (1).
3. `requirements_subset_append_of_unique_last`: `mem_requirements` on both sides; the only
   nontrivial disjunct is `rightMaximal w (A ++ B)`, discharged by (2) plus
   `rightExts_length_append`.
4. `chi_append_unique_last`: `chi_le_of_requirements_subset` (already banked in
   `SxgcChiMono`) with `positive (A ++ B) = positive A && positive B`.

## Gates

* **sorry-free**: `grep -cE 'sorry|^axiom|native_decide'` on the new file = 0.
* **`lake build` green**: full target set (`Sxgc`, `Main`/`sxgctest`, `SxgcBuild`,
  `SxgcBounds`, `SxgcPhi`, `SxgcNodup`, `SxgcRunEdge`, `SxgcSeam`, `SxgcRevival`,
  `SxgcChiMono`, `SxgcUniDelim`) — `Build completed successfully (6 jobs)`.
* **no new axioms**: `#print axioms` on all five headline theorems reports exactly
  `[propext, Classical.choice, Quot.sound]`; no `sorryAx`.
* **existing statements/definitions untouched**: `git status` shows only
  `M lean/lakefile.lean` (one appended `lean_lib SxgcUniDelim` line) and the new file
  `?? lean/SxgcUniDelim.lean`. `git diff lean/lakefile.lean` is a pure append. The retired
  false sorries in `Sxgc.lean` (`chi_from_events_REFUTED_AT_SCALE`,
  `witnesses_at_boundaries_FALSE_AS_STATED`) were not touched, and no file in the running
  data pipeline or any `.sxi` artifact was accessed.
* **changes left unstaged**, nothing committed (per lane rules).
* **non-vacuity checks** (kernel-evaluated `example`s in the file): the safe regime is
  inhabited (single-document append; unique non-separator delimiter), and the refutation's
  witnesses genuinely violate the hypothesis (`lossText.count 2 = 2`, `docLoss.count SEP = 5`
  — the separator recurs at every document end).

## Relation to the surrounding ledger

* `SxgcChiMono.chi_append_not_monotone` — χ-count is not monotone in general.
* `chi_single_symbol_document_decreases` — not even for a SEP-aligned single-symbol document
  appended to a *multi*-document `A`.
* **this file** — monotone again as soon as the trailing symbol is a genuine (unique)
  delimiter. The two statements are compatible and complementary: the document contract fails
  the hypothesis precisely because `0x1e` recurs at every document end, and the oracle hammer
  found violations need `A` with ≥ 4 documents, consistent with `chi_append_single_document`.

## Honest scope / follow-ups (statement-locked)

1. The **symmetric** oracle condition `p3` — "appending does not decrease χ when `last(B)` is
   unique in `A ++ B`" (3.0M pairs, 0 violations) — is **not** formalized here. It is a
   different route (the appended material's own unique delimiter), and it is the natural next
   target.
2. A full **characterization** of the safe regime (necessary and sufficient conditions on
   `last(A)`/`last(B)`) is not claimed; only the sufficient condition `p1` (plus its
   count/getLast form) is proved.
3. No minimality (shortest counterexample) or tightness claims are made anywhere in this file.

## Reproduction

```
cd <worktree>/lean
lake build SxgcUniDelim      # green
lake build                   # all targets green
```
