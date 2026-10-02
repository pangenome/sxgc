# Round 4: minimality lower bound — the saturated-regime wedge

## Outcome

**The live target sorry at `Sxgc.lean:7580` (inside `minimality`, under the
`REMAINING` comment) is unchanged** — one `sorry`, as before. `Sxgc.lean` is
byte-identical to HEAD (empty `git diff`), including both retired false
declarations (sorries at 5371/5654) and the completed covering proof.
**Sorry count at the target: 1 → 1. Total standalone sorries in `Sxgc.lean`:
3 (unchanged).** No new axioms; every new theorem audits clean.

The delivery is the route-(c) wedge: **O2 (no duplicates) is now a THEOREM
inside the entire saturated regime**, the boundary of that regime is now
exact, and the genuinely remaining obligations are statement-locked with a
proved assembly theorem:

```lean
theorem minimality_lower_of_O3_O4 (T : Text) (hT : positive T = true)
    (hO3 : O3_maximal T) (hO4 : O4_distinct T) (hO2 : O2_astronomic T) :
    (scan (T.length + 1) (triplesOf T)).length ≤ chi T
```

Combined with the proven `Sxgc.minimality_lower_of_scan_classes` and the
completed covering half, the lower half now needs exactly the three locked
statements `O3_maximal`, `O4_distinct`, `O2_astronomic` — and the third only
for texts longer than 2^63 - 1 characters.

## The wedge, made precise

The one-pass machine resets its running LCP minimum to the FIXED cap
`MAXINT = 9223372036854775807` after every BWT-run boundary (`scanAux`). The
round-3 duplicate families arise only where an artificially lowered cap cuts
below a stream's LCP values. The stream-level Nodup theorem
(`SxgcNodup.scan_nodup`, proven round 2) needs exactly
`∀ t ∈ ts, (t.lcp : Int) ≤ MAXINT` — the cap never binding.

The new kernel theorem `SxgcNodup.triplesOf_lcp_le` proves that for the
ACTUAL text stream every LCP value is at most the text length:

```lean
theorem triplesOf_lcp_le (T : Text) : ∀ t ∈ triplesOf T, t.lcp ≤ T.length
```

Route: the `lcp` field of row `i` is `lcpOf` of the suffixes of
`R = T.reverse ++ [0]` starting at the consecutive SA positions
`ord[i]!`, `ord[i-1]!` (extracted by the new `triplesOf_fields`);
`lcpOf` is bounded by the length of each argument (two new inductions);
consecutive SA positions are distinct (`saOrder_nodup` + `getElem_inj`),
so the shorter suffix is a *proper* suffix of `R`, of length at most
`|R| - 1 = T.length`.

Consequently the fixed cap can bind **only** when `T.length > MAXINT.toNat`,
i.e. for texts longer than 9,223,372,036,854,775,807 characters — that is the
exact saturation boundary, replacing round 3's qualitative suspicion. Inside
it, O2 is unconditional (no `positive` hypothesis, no per-stream LCP premise):

```lean
theorem scan_nodup_of_length_le (T : Text) (hlen : T.length ≤ MAXINT.toNat) :
    (scan (T.length + 1) (triplesOf T)).Nodup
theorem O2_bounded_premise_of_length_le (T : Text) (hlen : T.length ≤ MAXINT.toNat) :
    ∀ t ∈ triplesOf T, t.lcp ≤ MAXINT.toNat
```

The second discharges the boundedness premise of the locked
`SxgcBounds.O2_bounded` for every saturated text, so round 2's warning —
that the premise "does not follow from positivity" — is now resolved for all
physically realizable texts by following it from the text LENGTH instead.

Statement-locked residual (all in namespace `SxgcNodup`):

```lean
def O3_maximal   (T : Text) : Prop := ∀ x ∈ scan (T.length+1) (triplesOf T), IsMax T x
def O4_distinct  (T : Text) : Prop := ∀ x ∈ scan (T.length+1) (triplesOf T),
  ∀ y ∈ scan (T.length+1) (triplesOf T), ScopeLe T x y → ScopeLe T y x → x = y
def O2_astronomic (T : Text) : Prop :=
  MAXINT.toNat < T.length → (scan (T.length+1) (triplesOf T)).Nodup
```

Warrants, unchanged from rounds 1–3: O3/O4 have 0 counterexamples on the
729-text battery (`Sxgc.lean` in-file `#eval`s at the `obls` differential)
and 320 scratch texts; the round-3 small-cap families do not touch these
fixed-cap statements. `O2_astronomic` is believed true as well (the duplicate
mechanism needs three consecutive boundaries with all window LCPs and both
boundary LCPs above 2^63 - 1, i.e. a text of astronomic length with
astronomic-period repetitions); it is locked, not claimed.

A new in-file statement-lock eval accompanies the successor (SxgcNodup now
prints `saturation successor violations / 729: 0`, checking both Nodup and
the `lcp ≤ T.length` bound on the full 729-text battery).

## Why not sorry-count-at-target decrease

Discharging the target sorry requires O3+O4 proofs *inside* `Sxgc.lean`
(`SxgcNodup` imports `Sxgc`, so the target cannot consume downstream
theorems without an assembly step like the O1 campaign's). O3 (coverage
maximality of emitted run-edge positions) and O4 (distinct classes of emitted
positions) are full LCP-interval geometry arguments of the same scale as the
completed covering campaign; neither was attempted this round beyond its
statement-lock. No proof body of `minimality` was rewritten: with O3/O4 open
the branch would still need a sorry, and restating it as three sorries would
increase the count.

## Changed files

* `lean/SxgcNodup.lean` — 226 lines appended before `end SxgcNodup`; pure
  addition, no existing line touched. New declarations:
  `lcpOf_le_length_left`, `lcpOf_le_length_right`, `getElem!_map_range`,
  `triplesOf_fields`, `triplesOf_sa_lt`, `triplesOf_lcp_le`,
  `scan_nodup_of_length_le`, `O2_bounded_premise_of_length_le`,
  `O3_maximal`, `O4_distinct`, `O2_astronomic`,
  `minimality_lower_of_O3_O4`, private `satTexts`/`satOk` + one `#eval`.
* `lean/FLEET_MINIMALITY4_REPORT.md` — this journal.
* A scratch file used for iteration was deleted; no other file was touched.

## Validation

* Baseline `lake build SxgcNodup sxgctest`: GREEN before any edit
  (10 jobs, 6m25s), confirming the pre-change tree builds.
* `lake build SxgcNodup` after the edit: GREEN.
* `lake env lean SxgcNodup.lean`: clean elaboration; prints
  `"saturation successor violations / 729: 0"`.
* Axiom audits (`lake env lean` on an audit file importing `SxgcNodup`):
  all nine new theorems use only `propext`, `Classical.choice`, `Quot.sound`
  — no `sorryAx`, no `Lean.ofReduceBool`. `Sxgc.minimality` still reports
  `sorryAx` (the single live target sorry, unchanged).
* Concrete instantiation check: `scan_nodup_of_length_le [1,1,2,1] (by
  decide)` typechecks; `scan 5 (triplesOf [1,1,2,1]) = [2,3]`, `chi = 2` —
  a two-block family member on which the full-cap scan emits exactly χ
  positions (round 3's family showed `[3,4,4]` duplicates only under the
  artificial cap 1).
* `lake build` (default target): GREEN, 6 jobs; the only sorry warning is the
  target `Sxgc.lean:7560` declaration.
* BIT gate: `.lake/build/bin/sxgctest` — **511 pass, 0 fail,
  BIT 1B GATE: GREEN** (exhaustive {1,2}, |T| ≤ 8, covering + minimality).
* Source integrity: `git diff --stat` = 1 file, 226 insertions, 0 deletions;
  `git diff -- Sxgc.lean` is EMPTY; sorry lines at 5371/5654/7580 unchanged;
  `git diff --check` clean; **nothing staged**; no commits made.
* Retained artifacts (`/home/erikg/sxgc-466`, `/tmp/rpfbwt-64-466`, `.sxi*`,
  `/mnt/nvme3n1` refs) untouched and read-only; no processes killed; peak
  memory a few GB per lean process, far under the 64 GB budget. No long-run
  processes are alive at report time.

## Recommended next campaign

Attack `O3_maximal` and `O4_distinct` inside `Sxgc.lean` (assembly into the
target, per the O1 campaign pattern), using the proven row-geometry lane
machinery (`requirement_boundary`, `finite_convex_interval`, `wRow_lcp_*`,
`scan_emits_run_edges`, `runEdgeHit_true`, `runEdgeDominate_true`):
emitted positions are run-edge positions whose reverse contexts sit in
left- and right-closed LCP barriers — exactly the shape that makes a
coverage class maximal and single-member-per-class. `O2_astronomic` can be
deferred: it is not needed for any text that can exist.
