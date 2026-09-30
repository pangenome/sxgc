# O1 campaign report

## Result

The covering proof has been completed and assembled into the original,
statement-locked `Sxgc.covering_given_stream`. The stronger abstract theorem
`scan_hits_component` works for arbitrary triple streams, without an LCP
saturation bound. `scan_domination` follows, and `runEdgeDominate_true` is
proved in `SxgcRunEdge.lean`.

The lower half of `minimality` remains open. Its entire declaration and proof
body, including the existing sorry, are unchanged. A newly identified actual
text family exhibits duplicates under small scan caps; its extrapolation to
fixed `MAXINT` is not claimed as a kernel refutation.

Live target sorries: **2 → 1**. Total standalone sorries in `Sxgc.lean` and
`SxgcRunEdge.lean`: **4 → 3**. No new axioms or proof holes were introduced.
Both retired false declarations retain byte-identical statements and bodies.
All changes are unstaged. No commits, process kills, protected data paths, or
`.sxi` artifacts were involved.

The integrated `lake build` and targeted `lake build SxgcRunEdge` are
**GREEN**. Final axiom audits of covering, the component theorem, domination,
and RunEdgeDominate report only `propext`, `Classical.choice`, and `Quot.sound`,
with no `sorryAx`. The existing minimality theorem still reports `sorryAx`.

## Proof route

Read the three requested obstruction/assembly reports, the target contexts,
`counterexamples/saturation_refutation.lean`, and the oracle/constructor in
`tools/chi_rspace_proto.py`. The obstruction report predates the completed
`runEdgeHit_true` proof. The endpoint/event-restriction model is unnecessary
for this covering proof.

1. `requirement_boundary`: right-maximality supplies two adjacent rows in the
   reverse-context interval with different BWT characters; one character is
   the required `c`. The suffix case uses the zero-sentinel row. The remaining
   case uses two distinct right extensions.
2. `wRow_lcp_step` and `wRow_lcp_prev`: the stored adjacent-row LCP transports
   the reversed context exactly at the word-length threshold.
3. `finite_convex_interval` and `wRow_lcp_interval`: a context's sorted suffix
   rows form an inclusive interval `[a,b]`, with internal LCPs at least the
   word length and external barriers strictly below it.
4. `ThresholdClean`, `ThresholdPending`, and the block induction: after an
   entry barrier, the character's slot is below threshold, contains an
   appropriate pending witness, or has a below-threshold running minimum.
   An internal mixed boundary arms the character. Every overwrite retains
   an appropriate witness; an evaluation that could lose it emits it first.
   The exit barrier or final flush ensures emission. This argument does not
   assume `lcp ≤ MAXINT`.
5. `scan_hits_component`: list slicing and prefix-state preservation lift the
   block proof to the complete scan.
6. `scan_covers_requirement`, `covering_from_components`, and the original
   `covering_given_stream`: convert the emitted row back into a suffix of a
   text prefix. `scan_domination` uses a longest covered requirement, and
   `runEdgeDominate_true` restricts domination to the run-edge positions.

To avoid an import cycle, integration follows the earlier FmJoint assembly
pattern: the shared row-geometry lemmas from `SxgcRunEdge.lean` and the new
lane proofs are assembled in `Sxgc.lean` before the two final target theorems.
`runEdgeHit_true` remains in `SxgcRunEdge.lean`, alongside the new domination
result. Original theorem signatures are preserved, including moved helpers.
The temporary lane modules were removed after their contents were assembled;
ignored source snapshots remain under `.lake/o1-lane-sources/` for local audit.
No new lake targets or import-cycle workarounds are required.

## Dispatched worker briefs and results

The runtime exposes neither `contact_supervisor` nor `sxgc.dsv4-lean`. A
full-history spawn failed before starting; three fresh-context workers then
ran with the available inherited model. These were **not dsv4 runs**. No work
was assigned on criterion C, revival, or the seam lane.

All briefs required compilation or a precise missing-piece report, zero new
sorries/axioms, no dependency on the live target theorems or retired false
statements, and no staging/commits. The concrete exploratory hint was
`#eval triplesOf [1,1,2,1]`, with requirement `w=[1], c=1`.

### `o1_boundary`

Dispatched exact signature (namespace `Sxgc`):

```lean
theorem requirement_boundary (T : Text) (hT : positive T = true)
    (w : List Nat) (c : Nat) (hp : (w,c) ∈ requirements T) :
    ∃ k, 1 ≤ k ∧ k < (triplesOf T).length ∧
      charRow T (k-1) ≠ charRow T k ∧
      wRow T w (k-1) ∧ wRow T w k ∧
      (charRow T (k-1) = c ∨ charRow T k = c)
```

Result: exact statement compiled. Added a mixed-row boundary helper. Follow-up
investigation supplied the capped-scan actual-text saturation family and the
reproducible Python evidence artifact.

### `o1_lcp`

Dispatched exact signature:

```lean
theorem wRow_lcp_step (T : Text) (w : List Nat) (k : Nat)
    (hk : 1 ≤ k) (hkn : k < (triplesOf T).length)
    (hw : wRow T w (k-1)) :
    wRow T w k ↔ w.length ≤ ((triplesOf T).getD k ⟨0,0,0⟩).lcp
```

Result: exact statement, reverse transport, and full interval endpoint
characterization compiled. The worker also checked/fixed the root-authored
finite-interval and row-coverage helpers. Audits reported only
`propext`, `Classical.choice`, and `Quot.sound`.

### `o1_machine`

Dispatched exact signature:

```lean
theorem scan_hits_component (N : Nat) (ts : List Triple) (a b d c : Nat)
    (hab : a ≤ b) (hbn : b < ts.length)
    (hc1 : 1 ≤ c) (hc2 : c < SIGMA)
    (hin : ∀ j, a < j → j ≤ b → d ≤ (ts.getD j dT).lcp)
    (hleft : a = 0 ∨ (ts.getD a dT).lcp < d)
    (hright : b+1 = ts.length ∨ (ts.getD (b+1) dT).lcp < d)
    (hmix : ∃ k, a < k ∧ k ≤ b ∧
      (ts.getD (k-1) dT).c ≠ (ts.getD k dT).c ∧
      ((ts.getD (k-1) dT).c = c ∨ (ts.getD k dT).c = c)) :
    ∃ j, a ≤ j ∧ j ≤ b ∧ (ts.getD j dT).c = c ∧
      N - (ts.getD j dT).sa ∈ scan N ts
```

Result: exact unbounded statement compiled, after block-invariant and
indexed-slicing follow-ups. The root supplied the arbitrary-prefix and
low-entry lemmas. Axiom audit reported only the three standard Lean axioms.

## Remaining minimality obligation

There is no remaining O1 or RunEdgeDominate lemma. The exact missing lower
half remains:

```lean
-- Unproved obligation; not an additional declaration or axiom.
∀ (T : Text), positive T = true →
  (scan (T.length + 1) (triplesOf T)).length ≤ chi T
```

The existing `minimality_lower_of_scan_classes` reduces it to these precise
three facts, with `S := scan (T.length + 1) (triplesOf T)`:

```lean
S.Nodup
∀ x ∈ S, IsMax T x
∀ x ∈ S, ∀ y ∈ S, ScopeLe T x y → ScopeLe T y x → x = y
```

`SxgcNodup.O2_bounded_true` already proves the first fact only with the
additional hypothesis:

```lean
∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat
```

This hypothesis does **not** follow from `positive T`. It was not inserted
into the locked minimality statement. The new evidence makes unbounded O2,
and hence the locked list-length minimality target, suspect. A further
minimality campaign must resolve that issue before treating the bounded O2
proof as a usable unbounded input. No unproved FM/event bridge was used in
the completed covering proof.

## Actual-text saturation evidence

Artifacts: `counterexamples/o1_saturation_family.py` and
`counterexamples/o1_saturation_family.lean`.

For a variable cap `M`, let

```text
T_M = 1^(M+2) ++ [2] ++ 1^(M+1)
N   = 2M+5
```

Actual suffix sorting and LCP generation for every `M=0..10` agree with this
proposed stream formula:

- Initial character-1 rows have SA values `2M+4` down to `M+3`, with LCPs
  `0,0,1,...,M`.
- Next come `(c,lcp,sa)=(2,M+1,M+2)` and `(0,M+1,0)`.
- Final character-1 rows have SA values `1..M+1` and LCPs `M..0`.

The cap-`M` scan emits `[M+2, M+3, M+3]`. The first high boundary arms
characters 1 and 2. At the next high boundary, the saturated running minimum
prematurely emits both, then rearms character 2 at the same text position.
The subsequent lower boundary emits that marker again. With a sufficiently
large cap the output is `[M+2,M+3]`.

All eleven finite Python instances were executed successfully. The Lean
artifact also kernel-checks model fidelity (`scanCap MAXINT N ts = scan N ts`)
for every stream, and for positive `T=[1,1,1,2,1,1]` checks cap-1 output
`[3,4,4]`, `chi T = 2`, non-Nodup, and output length strictly above chi.
Its theorem audits use no axioms beyond `propext`. This is evidence
about an explicit cap-parameterized model and actual texts. The symbolic
extrapolation to fixed `MAXINT`, the full general suffix-sort formula, and
the general claim `chi T_M = 2` have not been kernel-proved here. The original
minimality theorem is therefore left unchanged, not relabeled as formally
refuted or replaced with a weaker theorem.

## Validation updates

- Baseline `lake build`: GREEN, 6 jobs, 347 seconds for `Sxgc`.
- All eight standalone lane modules compiled; no new sorry warnings.
- A source audit checked **247 existing theorem signatures**, all byte-identical
  across the two edited source files, allowing declarations to move.
- Both retired false statements and proof bodies: byte-identical.
- `minimality` statement and entire existing proof body: byte-identical.
- `git diff --check`: clean; Git index empty.
- Existing Bit 1B executable gate: **511 pass, 0 fail**, GREEN. The executable
  definitions are byte-identical in the integrated source.
- Final integrated `lake build`: **GREEN**, 6 jobs; `Sxgc` built in 379 seconds.
- Final `lake build SxgcRunEdge`: **GREEN**, 4 jobs.
- Final axiom audit: covering, requirement coverage, component hitting, scan
  domination, and RunEdgeDominate use only the standard three axioms; no
  `sorryAx`. The unchanged minimality proof still uses `sorryAx`.
- `lake env lean counterexamples/o1_saturation_family.lean`: **GREEN**; fidelity
  and modified-cap evidence audited separately.
- Final source landmarks: `scan_hits_component` at `Sxgc.lean:7393`,
  `covering_given_stream` at `Sxgc.lean:7554`, remaining `minimality` at
  `Sxgc.lean:7560`, and `runEdgeDominate_true` at `SxgcRunEdge.lean:240`.
- Accepted hard gate: one of the two live sorries eliminated, statements
  unchanged, zero new axioms, retired false bodies untouched, green build.
