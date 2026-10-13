# PiWall formalization report

Branch: `lean-pi-wall`. Baseline and final `lake build` pass under the
repository's unchanged `leanprover/lean4:stable` (resolved to v4.35.0).
Baseline cold compilation took about 318 seconds. Existing Sxgc warnings and
proof holes remain untouched. `lakefile.lean` only gains registrations;
PiWallEval and PiWallAxioms are additional default targets.

## Proven, axiomatized, open

- Model: `PiWall.lean:14`–`68`, permutation as forward/inverse functions,
  bounded list state, deterministic cyclic-stream enumerator, write-only
  output. Transition cannot access π or prior output; it sees only the
  current oracle item. Fuel is not available to the program. Space counts
  payload words with O(1) separate scalar control registers.
- **One explicit axiom**, `cycle_inversion_wall`, `PiWall.lean:86`:
  for n≥64, 1≤S≤n and every correct enumerator, some permutation/start
  forces at least floor(n²/(64S)) advances for every successful execution.
  Citations are in its marked block. This is a **model-transfer assumption**,
  not a proved consequence of the cited comparison-sorting results.
  The historical sorting paper's fifth author is Tompa, not Munro;
  Munro–Paterson's limited-storage paper is cited separately.
- Conditional lower endpoints: `wall_one`, `PiWall.lean:92`, and `wall_n`,
  `PiWall.lean:98`. **Both depend on the wall axiom.** An independent
  arbitrary-enumerator quadratic lower endpoint was not proved.
- Unconditional upper endpoints: `scanInverse_correct`, `PiWall.lean:170`,
  `denseInverse_correct`, `PiWall.lean:205`, whole-scan length and n-scan
  budget (`PiWall.lean:219`, `222`), and n-cell storage (`133`). Eval kernels
  are formally identified with these algorithms (`141`, `147`). The scalar
  budget is conservative; early-stop searches can take fewer observations.
  Lists describe streams/traces, not a claimed constant-memory Lean runtime.
  A refinement of both upper algorithms into the `Enumerator` transition
  machine has not been proved.
- **Periodic break proved without the wall axiom**:
  `equalRotation_iff_residue`, `PiWall.lean:309`; minimal p divides n (`342`);
  arithmetic classes have exact membership and ascending ties (`369`);
  p=n has only singleton ties (`385`); emitted rows are sorted, complete,
  injective (`449`, `472`, `501`). `periodic_break` (`517`) assembles valid
  output positions, cyclic rank order, coverage, and uniqueness.
  `list_rotation_iff_residue` (`580`) connects the theorem to finite list
  rotations. The theorem assumes an ordered p-entry representative table,
  matching C++'s already ordered classes, rather than assuming the residue
  classification. The executable `residueTable`/`cyclicPeriod` preprocessors
  are tested; their construction/minimality is not separately certified.
- Emission is five word-RAM operations per row (symbolic cost convention,
  not a Lean runtime-complexity theorem). Table storage is p **words** plus
  scalar counters. In bits the demonstrated representation is
  O(p log n + log n), **not O(p + log n) bits**. The latter would need a
  different representation and proof. Period discovery and representative
  sorting are excluded from the per-row claim and not bounded here.
- Run structure is defined by linear cyclic-BWT runs (`PiWall.lean:636`).
  The **OPEN PROBLEM** is documented at `644`, and `RunTradeoffQuestion`
  (`668`) packages its little-o bound question as an unasserted `Prop`.
  No axiom, proved existence claim, or proof hole is introduced for it.

## Validation

Commands (all passed):

```
cd lean
lake build                         # baseline and final (11 jobs final)
lake build PiWall PiWallEval PiWallAxioms
lake env lean PiWallEval.lean
lake env lean PiWallAxioms.lean
```

Eval output:

```
PI WALL ENDPOINT GATE: GREEN (5914 permutations, n=0..7; scalar n^2, dense n)
PERIODIC FIXTURE GATE: GREEN (24 texts; periods 1/2/3/7, copies 1..6; ties and full order)
PERIODIC EXHAUSTIVE GATE: GREEN (1022 nonempty binary texts, n=1..9)
CONVENTION GATE: GREEN (ascending ties, linear BWT runs, p=n singleton classes)
```

Source audit: zero `sorry`, zero `native_decide`, exactly one explicitly
marked axiom in PiWall.lean. The 35-theorem `#print axioms` audit shows only
wall_one/wall_n depend on cycle_inversion_wall; all constructive results use
at most propext/Classical.choice/Quot.sound, and none uses sorryAx.

## Operator's open problem

Can a uniform algorithm exploit at most r BWT runs to emit every position
in cyclic rank order with total time T(n,r) and peak state S(n,r) such that
T(n,r)S(n,r)/n² tends to zero for every family with n growing and r/n tending
to zero? Include preprocessing, samples, seam repair and all transient
state; specify text/BWT access and orientation. For ≤1 TB scratch, also
prove an actual peak-live-byte budget including disk intermediates, and a
usable emission/endpoint-locating time bound. A little-o product alone
promises neither that byte budget nor a wall-clock factor.

SA adds no information once BWT plus primary row/tie convention determines
the oriented text. Bare cyclic BWT can determine only a necklace; the
orientation qualification matters. The remaining question is computational.

**Speculation:** look for a cover of the position map by arithmetic
progressions or affine pieces, with O(r) anchors and fast rank-to-piece
selection. Periodic residue classes are an exact instance of such a cover.
LF's behavior inside a run suggests candidate pieces, but few BWT runs do
not establish a short arithmetic cover of its inverse, efficient piece
selection, or bounded seam conversion. None of these claims is proved.

## Coordination and commits

No contact_supervisor/intercom tool was present in the available tool
catalog; no supervisor messages or decision requests could be sent. No
ordinary-text status handoff was substituted.

Milestones: `4b5d980` (model/classification), `0cdac51` (upper endpoints and
periodic order), `3e47d80` (evals, open question and audit). The final report
and assembled periodic theorem/default-target registration are in the
following final milestone commit (see git log).
