# Fleet lane: Criterion C bridge (a) — primitive ⟹ distinct cyclic rotations

## Outcome

**Obligation (a) of the `g = 1` half of Criterion C is now Lean-proven, sorry-free and
axiom-clean.** If `byteFreqGcd T = 1`, then all `T.length` cyclic rotations of `T` are
pairwise distinct:

```lean
theorem byteFreqGcd_one_rot_injective (T : Text) (hg : byteFreqGcd T = 1) :
    ∀ s t, s < T.length → t < T.length → rot T s = rot T t → s = t
theorem rot_eq_imp_not_gcdOne (T : Text) (s t : Nat)
    (hs : s < T.length) (ht : t < T.length) (hst : s ≠ t)
    (h : rot T s = rot T t) : byteFreqGcd T ≠ 1
```

Obligation (b) — order preservation of the LF/Phi interval map (the affine
`phiFormula R u v x = phiInv R x` identity on a cyclic SA domain) — is **not** addressed;
it remains statement-locked and needs the suffix-array order model + r-space
spliced-output equivalence. Boundary is stated precisely below.

## What was added (`lean/SxgcPhi.lean`, new section "Criterion C bridge (a)")

Definitions:

* `rot T k = T.drop k ++ T.take k` — cyclic rotation by `k`;
* `CyclicSym T r := ∀ x, x < T.length → T[x]? = T[(x + r) % T.length]?` — a cyclic-shift
  symmetry (stated with `getElem?` so it is total; in range it is the pointwise shift
  identity).

Reusable list lemmas (no imports beyond `Sxgc`; the whole development is `List Nat`):

* `add_self_mod : (x + n) % n = x % n`;
* `length_flatten_replicate' : ((replicate m U).flatten).length = m * U.length`;
* `flatten_replicate_getElem? : ((replicate m U).flatten)[i]? = if i < m * U.length then
  U[i % U.length]? else none` (the power-witness index lemma).

Cyclic-group arithmetic, done by hand on `Nat` mod `n`:

* `cyclicSym_length` — `n` is always a symmetry;
* `cyclicSym_add` — symmetries closed under `+`;
* `cyclicSym_neg` — closed under complement `r ↦ n - r` (`r ≤ n`);
* `cyclicSym_sub` — closed under `a - b` (`b ≤ a ≤ n`), the descending operation;
* `cyclicSym_sub_mul` — the Euclidean descent `∀ k ≤ n/d, CyclicSym T (n - k*d)`;
* `cyclicSym_mod_eq` — a `d`-symmetry with `d ≤ n` makes every entry a function of
  its position mod `d`;
* `cyclicSym_isPower` — **strong induction on the shift** `d`: if `d ∣ n` the word is
  literally `(T.take d)^(n/d)`; otherwise the descent gives the symmetry `n % d`, which
  is smaller, and the induction hypothesis closes. This avoids `gcd`/Bézout and any
  group-theory library entirely — the only number theory is `Nat.mod_eq_of_lt`,
  `Nat.mod_add_mod`, `Nat.add_mul_mod_self_right` and `Nat.div_add_mod`.

Assembly:

* `rot_eq_cyclicSym` — equal rotations at shifts `s ≤ t` force `CyclicSym T (t - s)`
  (via the indexing lemma `rot_getElem?`: `(rot T k)[i]? = T[(i + k) % n]?`);
* `rot_eq_imp_not_gcdOne` / `byteFreqGcd_one_rot_injective` — the bridge, composing with
  the already-banked `gcdOne_not_isPower`.

The doc-comment of the preceding "Criterion C" section was updated (comment text only) to
record that bridge (a) is now discharged; **no existing statement or definition was
changed**.

## Executable cross-check (brute force, all texts over {1,2,3}, length ≤ 7)

```
texts=3280  gcd1=3003  gcd1-with-collision=0
collisions=54  collisions-not-power=0  gcd1-and-power=0
```

Every collision text is a proper power; no `gcd = 1` text has a collision; no power has
`gcd = 1`. (Script kept out of the tree; reproducible from the definitions above.)

## Remaining obligation (b), precisely

For primitive `R` the Nishimoto–Tabei interval map is exact: for run `i` with tail value
`u`, head value `v = phiInv R u`, and cyclic SA domain `D_i`, every `x ∈ D_i` satisfies

```lean
phiFormula R u v x = phiInv R x
```

Proving this needs (i) the suffix-array order model (tie-free suffix order under
primitivity) which `SxgcPhi` deliberately abstracts behind `PieceSound`, and (ii) the
r-space spliced-output equivalence (Phi anchors, run splicing). Both are absent here and
neither is asserted or stubbed: no `sorry` is introduced.

## Gates (all run in this worktree)

| Gate | Result |
|---|---|
| `lake env lean SxgcPhi.lean` | GREEN (no errors) |
| `lake build SxgcPhi` | GREEN |
| `lake build` (default; 6 jobs) | GREEN |
| `lake env lean --run Main.lean` | 511 pass / 0 fail; **BIT 1B GATE GREEN** |
| `lake env lean PhiAxioms.lean` | 6 new `#print axioms`; only `propext`, `Classical.choice`, `Quot.sound`; no `sorryAx` |
| `grep -c sorry SxgcPhi.lean` | 0 |
| `git diff --stat` | `SxgcPhi.lean` +274 / −7 (7 deletions are the comment-only doc update), `PhiAxioms.lean` +8 |

## Discovered regression (NOT from this lane)

`lean PhiEval.lean` is currently **broken by an ambiguity**: `rowPos` is defined in both
`Sxgc` (`Sxgc.lean:6311`, added by the O1 campaign commit `1e9cd3b`) and `SxgcPhi`
(`SxgcPhi.lean:25`), and `PhiEval.lean` does `open Sxgc SxgcPhi`. The ambiguity makes
elaboration fall back to `sorryAx`, so `#eval` aborts ("expression depends on the 'sorry'
axiom"). This is a pre-existing integration regression introduced by the O1 campaign, not
by this lane (PhiEval.lean is unmodified in `git diff`). Suggested one-line fix elsewhere:
qualify `rowPos` as `SxgcPhi.rowPos` in `PhiEval.lean` (out of scope here — no gate file
in this lane's remit was edited).

## Files changed (left UNSTAGED — nothing `git add`ed, nothing committed)

* `lean/SxgcPhi.lean` — new "Criterion C bridge (a)" section (additions) + comment-only
  doc update in the preceding section.
* `lean/PhiAxioms.lean` — 6 appended `#print axioms` audit lines.

No running data job or `.sxi` artifact was touched; no new axioms, no `native_decide`, no
re-`sorry`, and the retired false sorries in `Sxgc.lean` (~5364, ~5651) were not touched
(`Sxgc.lean` is unmodified).
