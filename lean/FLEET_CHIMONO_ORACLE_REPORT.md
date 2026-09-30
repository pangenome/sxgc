# χ-count monotonicity: ORACLE HAMMER — REFUTATION

**Lane:** `chi-count monotonicity - oracle hammer` (pure-Python brute force, no Lean edits).
**Date:** 2026-10-03. **Verdict: THE CONJECTURE IS FALSE — including under the 0x1e document contract.**

## Headline

> `chi(A ++ B) >= chi(A)` is **false**. It is false for arbitrary concatenation (≈1% of random
> pairs), and it is false **even for strict 0x1e document-contract texts with nonempty documents**.
> Minimal machine-verified counterexample (4 documents + 1 document):
>
> * `A = 1 ␞ 2 ␞ 2 ␞ 1 ␞`  (documents `[1] [2] [2] [1]`), `χ(A) = 4`
> * `B = 2 ␞`  (document `[2]`), `χ(A ++ B) = 3`
> * **χ decreases by 1**, confirmed both by the `chi_eq_maxClasses` characterisation and by
>   brute-force minimum-suffixient-set enumeration (`chi_min_bruteforce`, Lean's literal definition).
>
> A second contract-valid family exists with empty documents:
> `A = 1 ␞ ␞ ␞ 1 ␞` + `B = ␞` → `χ` 3 → 2 (brute-verified).

This also explains why the earlier 40k-pair search found nothing: **it only ever tested
single-document texts** (`A = body ++ [SEP]` with a SEP-free body). Those are safe (verified
below at ~5M pairs), and the decrease needs a *multi-document* `A` (≥ 4 documents in the minimal
witnesses found).

## Method and validation

Implementation: `/tmp/chi-mono-hammer/chimono.py` — faithful finite-text rendering of
`lean/Sxgc.lean`: `requirements` (right-maximal contexts: empty, suffix of T, or ≥ 2 right
extensions), `coversAt`/`covSet` (position x covers `(w,c)` iff the `|w|+1` window ending at x is
`w·c`), `ScopeLe`/`IsMax` (inclusion-maximal coverage sets), and `chi` via
`chi_eq_maxClasses` = number of distinct inclusion-maximal coverage sets.

Two independent validations, both zero-mismatch:
* vs. `bit6/sxi_logs/chunk-merge/monotonicity_probe.py` on all 63 texts `body ++ [SEP]`,
  `body ∈ {1,2}^{≤5}` — 0 mismatches;
* vs. **brute-force minimum suffixient set** (Lean's literal `chi`, enumerating position subsets)
  on 400 random texts and 200 random joined texts — 0 mismatches.

Scope: **linear** text definitions exactly as in Lean (`pref = take`); the cyclic/seam layer is a
separate model and is not claimed here. `SEP = 30`.

## Search coverage and results

| family | pairs | violations | min_delta |
|---|---:|---:|---:|
| exhaustive (F,F) α={1,2}, \|A\|≤9, \|B\|≤6 | 129,921 | **2,236** | −1 |
| exhaustive α={1,2}, \|A\|≤10, \|B\|≤6, single-doc aligned | 259,969 | 0 | 0 |
| exhaustive α={1,2,3}, \|A\|≤6, \|B\|≤4, single-doc aligned | 132,253 | 0 | 0 |
| exhaustive α={1,2,3,4}, \|A\|≤5, \|B\|≤3, single-doc aligned | 116,025 | 0 | 0 |
| exhaustive (sepA only / sepB only), α={1,2,3,4}, single-doc | 4 × ~120k | 0 | 0 |
| adversarial (powers, near-powers, mutated, run-length), single-doc aligned | 2.1M | 0 | 0 |
| random 500k × 5 configurations (single-doc aligned, incl. periodic, α≤4, \|A\|≤24) | 2.5M | 0 | 0 |
| sufficiency: p1 (last(A) unique in A) and p3 (last(B) unique in J), 6 × 500k | 3.0M | 0 | 0 |
| **multi-document, nonempty docs, maxdocs=2, doclen≤4** | 864,900 | 0 | 0 |
| **multi-document, nonempty docs, len≤8 exhaustive** | 1,069,156 | **10** | −1 |
| **multi-document, nonempty docs, len≤9 exhaustive** | 7,986,276 | **248** | −1 |
| **multi-document, nonempty docs, random large (maxdocs≤30)** | ~1.5M | **453** | −1 |
| multi-document, empty docs allowed, maxdocs=3, doclen≤2 | 159,201 | **≥1** (e.g. `1␞␞␞1␞`+`␞`) | −1 |
| A with exactly 1 document, B ≤ 3 documents | 88,620 | 0 | 0 |
| A with exactly 2 documents, B ≤ 3 documents | 2,658,600 | 0 | 0 |

**Every** violation observed anywhere has delta exactly **−1**.

## Exact statements that survive

* **True:** arbitrary append is not monotone (`A=(1,2,2,1)`, `B=(2,)`: 3 → 2).
* **True:** appending does not decrease χ whenever **last(A) is unique in A**, or **last(B) is
  unique in A++B** (3.0M random pairs + all applicable exhaustive pairs, 0 violations). These are
  exactly the conditions under which the boundary symbol is a genuine delimiter:
  - all single-document aligned tests satisfy one of them → explain the earlier "safe" readings;
  - the document contract satisfies neither in general, because SEP recurs at every document end.
* **False:** the claim that the contract *implies* monotonicity. The 4-document witness is a
  perfectly well-formed contract text.

## Mechanism (why the count drops)

From the minimal witness:

```
A = (1,␞,2,␞,2,␞,1,␞)          χ = 4 distinct maximal classes
  pos3 (sym 2, doc [2]) : {1␞>2, 2␞>2, eps>2}   MAX
  ...
J = A ++ (2,␞)                  χ = 3
  pos3 (sym 2, doc [2]) : {2␞>2, eps>2}          (demoted — contained in pos5's class)
  pos5 (sym 2, doc [2]) : {1␞2␞>2, 2␞2␞>2, 2␞>2, eps>2}  MAX
```

* In `A`, the context `1␞` was right-maximal **only because it is a suffix of A** (it has a single
  right extension, `2`). Hence `(1␞, 2)` was a requirement, and position 3's coverage set was
  maximal.
* Appending `B` makes `1␞` no longer a suffix of the text (the text now ends in `2␞`), and it still
  has one right extension → the requirement `(1␞, 2)` **disappears**. Position 3 loses it, its
  coverage set shrinks into position 5's, and its maximal class is **demoted with no compensating
  creation** (the appended text contributes classes, but not enough to offset).
* Generalisation of the mechanism: **loss of "terminal-context" requirements** (`w` that was
  right-maximal only as a suffix of `A`) demotes maximal classes; the appended material does not
  always create a matching number of new maximal classes.
* Among all 10 exhaustive minimal violations: `revived = []` in every case, `new_max ≥ 1` in every
  case, yet χ still drops — i.e. **count decreases are never revival-driven; they are
  demotion-driven.** This is independent of the earlier revival result (the witness *set* is
  non-monotone in both directions; the *count* is not monotone either, but for a different reason).

## Alternatives tested (boundary map)

*(all 0 violations)*: `A` ends with `0x1e`, `B` arbitrary/has no trailing `0x1e`; `A` arbitrary,
`B` ends with `0x1e`; both end with `0x1e` — **but only when at most one document is present**.
The condition "at least one of `A`,`B` ends with the delimiter" is **sufficient only in the
single-document regime**; it fails as soon as `A` contains ≥ 4 documents (and the delimiter recurs).

*(violations)*: delimiter also occurring inside the content (whether or not the text ends with it);
`A` with ≥ 4 documents; empty documents adjacent to a document boundary.

## Consequences

1. **No incremental χ-count invariant is available.** The chunk-merge / progressive-construction
   design must not assume χ only grows; the previously banked architecture decision ("derive χ once
   from the final merged structure") stands and is now **strengthened** — there is no sound cheap
   lower-bound bookkeeping to exploit, and any prune-based incremental χ maintenance is unsound.
2. **Theory ledger:** record as a negative result. The prior claim "40k pairs, no decrease" was
   true but vacuous (single-document family). The surviving positive statements are the two
   sufficient conditions (unique trailing symbol), which are worth proving in Lean as they *do*
   hold and characterise exactly the "one extra document appended to a one-document text" case.
3. **If a merge invariant is wanted**, it must be explicit compensation tracking (e.g. per-demoted
   class, a distinct newly-created class), not χ-count monotonicity — and the minimal witnesses
   above show any such invariant must be weak (χ can genuinely drop by 1).

## Reproduction

```
cd /tmp/chi-mono-hammer
python3 chimono.py --self-test           # vs monotonicity_probe
python3 chimono.py --self-test2          # vs brute-force minimum suffixient set
python3 hammer.py exhaustive|adversarial # probe search families
python3 contract3.py                     # true multi-document contract (finds violations)
python3 vs3.py                           # minimal violation set + statistics
```

Minimal witness, one line (both definitions):

```
A = (1,30,2,30,2,30,1,30); B = (2,30);  chi(A) = 4;  chi(A+B) = 3
```
