# Chi-count monotonicity: proof-lane report

2026-09-30. Main checkout: `/home/erikg/sxgc`.

**The conjecture is false for the exact definitions in `Sxgc.lean`, including
SEP-aligned append of a single-symbol document.** This lane provides kernel
proofs of decreases, a conditional class-count theorem, and positive special
cases. All 40 new theorems are sorry-free and have no dependency on `sorryAx`.

Files delivered:

- `lean/SxgcChiMono.lean` (new library; imports and reuses `SxgcRevival`/`Sxgc`).
- `lean/lakefile.lean` (one appended `lean_lib SxgcChiMono` entry).
- This report.

Everything is **unstaged**. No commits, staging, data-job changes, or `.sxi`
accesses were made. No parallel agents or oracle searches were launched.

## What closed

The exact statement is recorded as a proposition, not an axiom:

```lean
def ChiAppendMonotone : Prop :=
  ∀ T₁ T₂ : Text, chi T₁ ≤ chi (T₁ ++ T₂)

theorem chi_append_not_monotone : ¬ ChiAppendMonotone
```

Three explicit counterexamples are proved:

| Case | Old text | Append | Certified chi |
| --- | --- | --- | --- |
| Unrestricted positive text | `[1,2,3,1,3,1,2]` | `[3]` | `4 -> 3` |
| SEP-aligned single-symbol document | `[1,30,3,4,30,3,30,4,30,5,1,30]` | `[3,30]` | `7 -> 6` |
| Append a literal prefix of the old text | `[3,30] ++` the preceding old text | `[3,30]` | `7 -> 6` |

Here SEP is the existing `SxgcRevival.SEP = 30`. The second old text represents
five nonempty documents: `[1]`, `[3,4]`, `[3]`, `[4]`, `[5,1]`. The appended
sixth document is `[3]`. The third example adds a leading document `[3]` and
appends another `[3]`; Lean also proves `[3,SEP] <+: prefixLoss`.
All examples lie in the existing positive alphabet domain.
No minimality of the counterexample lengths is claimed.

Relevant theorems:

- `chi_lossText`, `chi_lossJoined`, `chi_append_decreases`,
  `chi_append_not_monotone`, `loss_class_counts`.
- `chi_docLoss`, `chi_docJoined`, `chi_single_symbol_document_decreases`.
- `chi_prefixLoss`, `chi_prefixJoined`, `chi_prefix_extension_decreases`.

Thus requested special case (a) is false. Case (b), interpreted explicitly as
“the appended text is a prefix of the old text,” is also false. If “prefix
extension” merely means the old text is a prefix of the joined text, the
unrestricted counterexample already refutes it.

## Kernel certificates for the document examples

The larger examples do not evaluate the exponential powerset definition of
chi or trust an external representative counter. `chi_lower_certificate`
proves a generic bound: k occurring requirement words that are pairwise
suffix-incomparable force k different witnesses in every suffixient cover.
It uses `count_le_of_disjoint_witnesses`, `reps_suffixient`, and the existing
`chi_eq_maxClasses`. Finite facts are checked by kernel `decide`.

For `docLoss`, seven such words are:

```text
[1]
[1,30,3]
[30,3,4]
[4,30,3]
[30,3,30]
[30,4]
[4,30,5]
```

The actual suffixient cover `[1,3,4,6,7,8,10]` supplies the matching upper
bound, proving chi = 7. After append, `[1,4,6,8,10,14]` is a suffixient cover;
six pairwise suffix-incomparable requirements prove the matching lower bound,
so chi = 6. Corresponding certificates prove both exact values for
`prefixLoss`. There is no native-evaluation proof tactic.

## The obstruction, precisely

The repository definitions are **finite-text**, not cyclic. `coversAt wc x T`
tests whether wc is a suffix of `T.take x`; it does not wrap. The theorem
`coversAt_append_old` proves that this word-coverage predicate is unchanged
at every old position x <= |T1|. What changes is which words are requirements.

`lost_requirement_is_terminal` proves the following necessary condition:
if `(w,c)` is an old requirement but not a joined requirement, then w is
nonempty, w was a suffix of the old text, and its old right-extension list
had length < 2. Since c belongs to that list, it has exactly one member.
Occurrences and right extensions themselves persist under append; terminal
right-maximality need not persist. The theorem states a necessary condition,
not an unsupported iff characterization.

The unrestricted example makes the failed compensation step explicit:

- `([1,2],3)` is an old requirement and disappears after append.
- Old maximal position 3 is strictly dominated by position 5 in the joined
  text (`lost_class`). Two old symbol-3 classes can merge into one.
- `([1,2,3],1)` is a new requirement. It strengthens position 4, which was
  already maximal and remains maximal (`strengthened_class`). Strengthening
  that existing class supplies no compensating increment in the count.

A merge target being a surviving class therefore does **not** imply no net
loss: multiple old classes may share that target. Likewise a new requirement
need not create a new maximal class. Exact counts certify the net loss.

The restriction/repair route is also refuted, without needing wrapping:
`local_cover_does_not_restrict` proves `[4,5,7]` is suffixient for the joined
text, every one of these positions is old-local, and the same set is not
suffixient for the old text. `no_three_witness_repair` proves **no** list of
at most three old positions can repair it. Thus a cardinality-preserving
restriction/replacement lemma cannot hold generally.

## Positive results banked

The doc-comment in the new file states the precise conditional dynamics
lemma. Its proof closes:

1. `longest_words_disjoint`: if longest requirements from two old canonical
   maximal representatives are both covered by one position in **any** text,
   those representatives are equal. The position need not belong to the old
   text. The argument uses suffix comparability and old maximality.
2. `maxClassCount_le_of_surviving_longest`: if each old representative has a
   longest covered requirement that remains a requirement of J, every
   suffixient set S of J has length at least `maxClassCount A`.
3. `chi_le_of_requirements_subset`: requirement inclusion implies chi
   monotonicity, even for unrelated texts A and J.

The following unrestricted-in-length special cases follow:

- `chi_append_fresh`: append `d :: B` with d absent from the old text.
  Every old terminal requirement acquires a distinct second right extension
  at the seam, so every old requirement survives. B is arbitrary.
- `chi_append_disjoint`: fully disjoint old/appended alphabets, including
  empty append.
- **Requested case (c)**: `chi_append_disjoint_content` allows shared SEP;
  the alphabet intersection is contained in `{SEP}` and the first appended
  symbol d is not SEP. This covers SEP-separated corpora with a nonempty
  first appended document. Empty/leading-empty-document variants are not
  silently included in this theorem's statement.
- `chi_append_fresh_document`: `[c,SEP]` when c is absent from the old text.
- `chi_append_of_suffix`: the old text is also a suffix of the joined text,
  so old terminal contexts stay terminal. `chi_append_self` specializes this
  to doubling a text.

The chi comparison theorems state the existing `positive` hypotheses needed
to apply `chi_eq_maxClasses`. The occurrence, requirement-preservation, and
conditional maximal-class lemmas do not redefine the model or its domain.
No general compensation lemma remains as a sorry: its intended universal
consequence has been refuted. No new open theorem is asserted.

## Verification and preservation gates

All commands below completed successfully (using `LEAN_NUM_THREADS=4`):

```text
lake -d /home/erikg/sxgc/lean build SxgcChiMono
  Build completed successfully (5 jobs).

lake -d /home/erikg/sxgc/lean build
  Build completed successfully (6 jobs).

lake -d /home/erikg/sxgc/lean build Sxgc SxgcBuild SxgcBounds SxgcPhi LM \
  LowerBound SxgcNodup SxgcRunEdge SxgcSeam SxgcRevival SxgcChiMono sxgctest dbg
  Build completed successfully (30 total build jobs, not 30 concurrent processes).
```

An automatically enumerated `#print axioms` audit covered **all 40 new
theorems**. The union of dependencies is only Lean's standard
`propext`, `Classical.choice`, `Quot.sound`; there is no `sorryAx` or
native-evaluation axiom. The new library has no new warnings or proof holes.
Existing imported libraries still emit their pre-existing warnings.

Build/audit logs from this run:

```text
/tmp/chimono-target-build.log
/tmp/chimono-full-build.log
/tmp/chimono-all-targets-build.log
/tmp/ChiMonoAxioms.lean
/tmp/chimono-axioms.log
```

`git diff --check` passed. `git diff --cached --name-only` was empty.
All existing Lean source statements/definitions are unchanged. In particular,
working-tree hashes match HEAD byte-for-byte:

```text
lean/Sxgc.lean        e85ac43af870e9109e8f6b8d4f5ff1712758c30d
lean/SxgcRevival.lean 02d7a70bc4ff6b1e0f2d6e5a5ac7190b5f5271b4
```

## Journal and oracle delta

- Inspected the definitions and immediately separated finite `coversAt`
  semantics from the cyclic-frame warning in the task.
- Traced terminal-requirement loss and constructed the 4 -> 3 example by
  hand; confirmed that explicit instance in Lean, then proved the decrease.
- Extended that mechanism to SEP-aligned documents and to prefix append.
  Constructed lower-bound certificates and proved exact 7 -> 6 counts.
- Completed conditional class dynamics and the disjoint/fresh/border special
  cases, then ran the target, default, all-target, and full axiom gates.
- Final oracle check: 2026-09-30 22:00 UTC. The separate directory
  `/tmp/chi-mono-hammer` contained only `chimono.py`; its self-test process
  was still running at the last process check. No oracle report,
  counterexample, or equality-case dataset had landed in that directory or
  in the repository. Consequently this report does not claim oracle search
  coverage, minimality, equality statistics, or independent agreement.
  The old `chunk-merge/monotonicity_probe.py` was read only; no search was run.

`contact_supervisor` was unavailable: discovery in the supplied callable tool
catalog returned no match. This report was written during the campaign to
expose the critical decreases in the shared main checkout. No clarification
or permission was needed to complete the requested proof work.
