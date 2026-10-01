# Round 3: minimality lower bound and saturation

## Outcome

**The main theorem is NOT completed.** The lower-half sorry at
`Sxgc.lean:7580` remains unchanged. `Sxgc.lean` itself is byte-identical to
HEAD, including both retired false declarations and all existing statements.
The completed covering proof is untouched. No new axiom or sorry was added.

The bankable result is a sharper obstruction: a kernel-checked local
emit/rearm characterization, a proof that positive actual texts have
arbitrarily large LCPs, and actual-text modified-cap examples separating all
three obligations O2/O3/O4. The family analysis below explains exactly why
the full-length-cap experiments do not address the locked, fixed-MAXINT
theorem. I have not kernel-refuted the locked text theorem.

The session exposed no `contact_supervisor` tool and no requested
`lunaroute/deepseek-4.1-flash` worker model. Work was performed solo. No
processes were killed; the work used at most a few concurrent processes and
did not touch the milestone or chunk-merge lanes. Changes remain unstaged.

## Checked Lean results

All additions are appended to
`counterexamples/o1_saturation_family.lean`, after its original contents.
Its existing declarations are unchanged.

`singleton_emit_rearm_iff` proves, for an in-range slot of sufficient table
size with stored length `a`, the equivalence

```lean
(min cap (b : Int) < (getR R c).len ∧
  (getR (upd (evalStep (min cap (b : Int)) R out).2 c b x) c).active = true)
  ↔ cap < (a : Int) ∧ cap < (b : Int)
```

The left side deliberately includes the reset test. It is an actual
emission only when the old slot is active. The companion theorem
`singleton_saturation_emits_and_rearms` assumes
`getR R c = ⟨a, x, true⟩` and proves both that `x` is in the evaluated output
and that the updated slot is exactly `⟨b, x, true⟩`.

This is the problematic transition at the two consecutive boundaries of a
singleton BWT run: the running minimum was just reset to the cap, so the
exit evaluation uses `min cap b`. Both adjacent LCPs exceeding the cap
causes premature emission followed by rearming of the same owner position.
For a general stream, a rearmed candidate might later be overwritten;
these local lemmas do **not** claim that every rearming necessarily yields
a second eventual emission. In the two-block family, character 2 has no
later update, so its rearmed candidate survives to emission.

The new theorem

```lean
positive_text_has_large_lcp (n : Nat) :
  ∃ T : Text, positive T = true ∧ ∃ t ∈ triplesOf T, n < t.lcp
```

uses `T = List.replicate (n+2) 1` and the requirement
`(List.replicate (n+1) 1, 1)`. The proven `requirement_boundary` and
`wRow_lcp_step` give a true suffix-stream LCP at least `n+1`, without
constructing or evaluating an enormous suffix array. Consequently:

```lean
positivity_does_not_bound_lcp :
  ¬ (∀ T : Text, positive T = true →
      ∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat)
```

This rules out deriving `SxgcNodup.O2_bounded_true`'s extra premise from
positivity. It does not itself refute unbounded O2 or minimality.

Additional kernel examples:

* Fixed-cap **arbitrary stream** with distinct in-range SA values:
  `scan 4 fixedCapStream = [1,2,2]`. Its LCPs exceed `MAXINT`; it is explicitly
  not claimed to be a valid four-row text stream.
* Actual positive text `[1,1,2,1,1]`, cap 0: output `[2,3,3,5]`, chi 2.
  Positions 2 and 5 are both maximal and mutually `ScopeLe`. Even deduping
  positions leaves length 3, strictly larger than chi. This isolates O4 in
  addition to the O2 failure.
* Actual positive text `[1,1,2,1,1,1]`, cap 0: output `[2,3,3,6]`.
  Position 2 is strictly dominated by position 6, and is not `IsMax`.
  This isolates O3 as another possible saturation failure.

## Two-block family characterization

Take `a,b ≥ 1`, nonnegative cap `M`, and

```text
T(a,b) = 1^a ++ [2] ++ 1^b
N      = a+b+2
A      = a-1
B      = min(a,b)
C      = b-1
```

The proposed full suffix stream is:

1. For `r=0..a-1`: `(c,lcp,sa)=(1,max(0,r-1),a+b+1-r)`.
2. `(2,A,b+1)`.
3. `(0,B,0)`.
4. For `k=0..b-1`: `(1,b-k-1,k+1)`.

There are exactly three mixed boundaries. At the first, characters 1 and
2 are armed at positions `a` and `a+1`, with length `A`. At the second,
the evaluation threshold is `min(M,B)`; at the third it is `min(M,C)`.
Subsequent rows all have character 1 and cannot cause another update.

The resulting multiplicity formulas are:

```text
count(a+1) = 1 + [M < min(a-1,b)]
number of character-1 emissions = 1 + [M < min(a-1,b-1)]
|scanCap M| = 2 + [M < min(a-1,b)] + [M < min(a-1,b-1)]
```

Here brackets denote 0/1 indicators. Character-1 emissions can only be at
`a` and `a+b+1`, at most once each; both occur exactly under the second
indicator. The only duplicate position is the marker `a+1`, and it is
duplicated exactly under the first indicator.

When both unary positions are emitted:

* `a=b`: they are distinct representatives of the same maximal class.
* `a<b`: position `a` is strictly dominated by `a+b+1`.
* `a>b`: position `a+b+1` is strictly dominated by `a`.

Mathematically chi is 2 for every such text. Every requirement ending in 2
is covered by its unique position `a+1`. Requirements ending in 1 have
unary contexts: a context containing the unique 2 cannot both be
right-maximal and have a right extension. The endpoint of a longest unary
block covers every unary requirement. The two empty-context requirements
force at least two positions. This paragraph and the general family
formulas are mathematical analysis and finite-checked conjectured Lean
lemmas, **not general kernel theorems in this delivery**.

The original O1 family is `a=M+2, b=M+1`: the first indicator is 1 and the
second 0, hence output `[M+2,M+3,M+3]`. At cap at least `min(a-1,b)`, both
indicators vanish. In particular a cap equal to the full text length
eliminates these saturation effects. The executable `scan` still resets to
the fixed integer 9223372036854775807; it does not reset to text length.

## Precise remaining obligations

The locked lower half remains exactly:

```lean
∀ T : Text, positive T = true →
  (scan (T.length + 1) (triplesOf T)).length ≤ chi T
```

The existing class reduction needs these three statements, with no extra
premise allowed in the final locked target:

```lean
∀ T : Text, positive T = true →
  (scan (T.length + 1) (triplesOf T)).Nodup

∀ T : Text, positive T = true →
  ∀ x ∈ scan (T.length + 1) (triplesOf T), IsMax T x

∀ T : Text, positive T = true →
  ∀ x ∈ scan (T.length + 1) (triplesOf T),
  ∀ y ∈ scan (T.length + 1) (triplesOf T),
  ScopeLe T x y → ScopeLe T y x → x = y
```

The small-cap evidence is not a direct counterexample to these fixed-cap
statements. It makes their unbounded extension suspect, and the new
large-LCP theorem eliminates the obvious way to bypass that concern.
An injection from emitted **positions as a set** would not suffice to
bound the locked **list length** unless multiplicities are also handled.

A concrete route to settle the saturation objection is to prove these
three family statements (signatures only, no new axioms or declarations):

```lean
-- Let family M := List.replicate (M+2) 1 ++ [2] ++ List.replicate (M+1) 1.
-- Let familyStream M be the closed form in the O1 report / Python artifact.
∀ M : Nat, triplesOf (family M) = familyStream M

∀ M : Nat, Sxgc.SaturationEvidence.scanCap (M : Int)
  (2*M+5) (familyStream M) = [M+2,M+3,M+3]

∀ M : Nat, chi (family M) = 2
```

Combined with the already proven `scanCap_MAXINT`, instantiating at
`MAXINT.toNat` would kernel-refute the locked minimality target. The first
statement requires the symbolic insertion-sort/suffix-order proof; the
second requires lifting the checked local transition through symbolic
same-character prefixes and tails; the third requires formalizing the
two-position cover argument above. None is asserted as proved here.

## Validation

* `lake build`: GREEN, 6 jobs; `Sxgc` compiled in 390 seconds. This retains
  the three existing sorry warnings (two retired, one live).
* `lake env lean counterexamples/o1_saturation_family.lean`: checked
  separately because counterexample files are not default lake targets.
  Axiom audits cover the symbolic transition, large-LCP theorem, and
  concrete obstruction examples; only `propext`, `Classical.choice`, and/or
  `Quot.sound` are used, with no `sorryAx` or `Lean.ofReduceBool`.
* `python3 counterexamples/minimality_saturation_audit.py`: GREEN.
  **8,800** family instances (`a,b=1..20`, caps `0..21`) verify the actual
  suffix stream and exact multiplicities. Coverage sets are independently
  derived from text requirements, rather than the scan/FM oracle.
* The same audit checks **2,047** distinct binary texts of length at most
  10 at cap equal to text length: covering, O2, O3, O4, and equality with
  the maximal-class count all pass.
* Original executable Bit 1B gate and final source/index checks are
  recorded in the final validation note below.

Final validation note: the separate Lean artifact check exited 0, including
`equalBlocks_maximal` and `equalBlocks_dedup_still_exceeds_chi`. All printed
axiom audits are clean. The original `sxgctest` executable reports **511
pass, 0 fail, BIT 1B GATE: GREEN**. Byte comparisons against HEAD confirm
that both `Sxgc.lean` and `SxgcRunEdge.lean` are unchanged and the original
evidence artifact is an exact prefix of its extended version. `git diff
--check` is clean; the Git index is empty. The only changed/new tracked
source artifacts are this journal, the extended Lean evidence file, and
the new Python audit script.

The theorem-completion gate is not met: the requested sorry was deliberately
left in place rather than using the now-refuted positivity-to-LCP-bound
bridge, importing a retired false theorem, or changing the statement.
