# FLEET_SEAM_REPORT — seam identity second half (reordering-inside-classes)

Lane: `seamHalf2` (native runner; the codex/`deepseek-4.1-flash` attempt died with
`400 The 'deepseek-4.1-flash' model is not supported when using Codex with a ChatGPT account`,
so this lane was re-run natively). All changes left UNSTAGED; nothing committed.

## Outcome in one line

The **strict-total-order core** and the **seam-class structure** of the
reordering-inside-classes half are now Lean-proven (sorry-free, no new axioms);
the file's open proposition `seamRepair_reorders_in_classes` is shown to be
**vacuous as literally stated**; the genuinely remaining content is stated
precisely as the gap below.

## What is the "second half" (from the file docstring)

> "The reordering-inside-classes half (sorting by exact cyclic LCE, ties by
> increasing position, equals the independent cyclic oracle) additionally needs
> the r-space sampling model (Phi anchors, run splicing) and is statement-locked
> below as an open proposition with its measured warrant."

i.e. the target is the open proposition `seamRepair_reorders_in_classes`, whose
intended content is that the repair's class sort equals the cyclic rotation order
restricted to the class.

## Statement correction (important)

`seamRepair_reorders_in_classes` is, **as literally written**:

```
∀ T i j, i ≠ j → (suffixAt T i <+: suffixAt T j ∨ suffixAt T j <+: suffixAt T i)
        → (cycCmp T i j ∨ cycCmp T j i)
```

Its prefix-comparability hypothesis is **unused**, and its conclusion is exactly
`rotation_cmp_total` — already proven in the file. So the proposition as stated is
**implied by totality alone** and does not encode the reordering content. This is
recorded as `seamRepair_reorders_in_classes_from_total`, so the trivial derivation
is not mistaken for the substantive half. (Statement-level observation, in the
spirit of the project's corrections ledger.)

## What was proven (all sorry-free, 204 added lines, no existing statement/def edited)

Order core — `cycCmp` is a strict total order on rows:

- `lexLt_irrefl` — first-difference order is irreflexive.
- `lexLt_asymm` — and asymmetric.
- `lexLt_trans` — and transitive (three-way prefix-length case analysis;
  `List.take_append` / `take_of_length_le` scrub).
- `cycCmp_irrefl`, `cycCmp_asymm`, `cycCmp_trans` — hence the cyclic comparison is
  irreflexive, asymmetric, transitive.
- `cycCmp_decides : i ≠ j → (cycCmp i j ∧ ¬ cycCmp j i) ∨ (cycCmp j i ∧ ¬ cycCmp i j)`
  — with `rotation_cmp_total` this is **trichotomy**: the comparison decides every
  distinct pair. This is exactly "the cyclic comparison is a strict total order on
  the class rows", so **sorting a class by it is well-defined and its result is
  unique** — the order-theoretic content of the half.

Class structure:

- `compRel` — distinct prefix-comparable rows (the seam-class relation).
- `comparable_seamPos` — comparable rows are exactly seam rows (closure on both
  endpoints).
- `nonseam_incomparable` — a non-seam row is prefix-incomparable with every other
  row. Consequence: non-seam rows are pairwise incomparable, are singleton classes,
  and the hypotheses of the proven `identity_outside_classes` are automatic for
  them; no reordering can displace one.

- `seamRepair_reorders_in_classes_from_total` — the statement correction above.

## The precisely remaining gap (not closed here)

1. **Class-partition structure.** "Terminal-suffix prefix intervals are nested or
   disjoint, and their maximal union partitions the ambiguous rows." Prefix-
   comparability is *not* transitive, so the classes are the connected components
   of `compRel`, not `compRel` itself; formalizing the nested/disjoint interval
   partition needs a new definition (prefix intervals of the suffix trie) — the
   connective closure relation. This is expressible here but is a further building
   block, not a one-lemma gap.
2. **r-space spliced-output equivalence.** That the r-space output (Phi anchors,
   run splicing, coalescing) sorted by this order equals the independent cyclic
   oracle restricted to the class. This needs the r-space sampling model, which is
   **not in vocabulary** in this file (no anchors/splicing definitions), so it
   cannot be stated, let alone proven, without new machinery — out of scope for a
   narrow lane and deliberately left statement-locked.
3. **Measured warrant** (unchanged): yeast235, seven maximal classes, 13,503
   candidate rows, repaired output byte-identical to the dense independent cyclic
   oracle, identical endpoint tie order. The empirical anchor for (2).

## Definition of done / gates

- `lake build` green end-to-end (`SxgcSeam`, `Main`, `sxgctest`) — PASS.
- Zero `sorry` / `axiom` / `native_decide` in `SxgcSeam.lean` — PASS (grep = 0).
- No existing theorem statement or definition edited; only additions — PASS
  (`git diff --stat`: 204 insertions, 0 deletions).
- Changes UNSTAGED, nothing committed — PASS.

## Evidence of the statement correction (reproduce)

```lean
example : seamRepair_reorders_in_classes := by
  intro T i j hij _
  exact rotation_cmp_total T i j hij
```
Compiles — confirming the open proposition is implied by totality with its proper
hypothesis unused.

## Recommendation

Merge the order core (it is new, correct, and closes the order-theoretic
requirement of the half). Keep the two substantive items (1)/(2) statement-locked
or route them to a dedicated r-space-model lane; do not "close"
`seamRepair_reorders_in_classes` as if it established the half — it does not.
