# Phi construction lane acceptance — 2026-09-27

**GREEN for the requested formal and small-text differential gates.**
Base `1cfef7d`; additive work in `/tmp/sxgc-laneT`; no commits or staging.

## Formal deliverable

[SxgcPhi.lean](SxgcPhi.lean) imports SxgcBuild and is registered as a new
`lean_lib`. Nineteen theorems, zero proof holes. Positions and rows are zero
based in the indexed text. `phi`/`phiInv` use the actual `saOrder` permutation,
with cyclic predecessor/successor; rank zero and singleton cases are covered.
This cyclic permutation is intentionally distinct from Sxgc's partial PLCP
`phiOf` definition.

- `headFromTail`: for `0 < a < R.length`, `SA[a] = phiInv(SA[a-1])`.
- `runHeadFromTail`: explicit BWT-run-boundary and head/tail sample contract.
  `RunBoundary` is precisely a valid nonfirst row with a preceding character
  change, so the predecessor row ends the preceding run.
- `phiInv_phi` and `phi_phiInv`: both permutation inverse laws on valid positions.
- `headFromCyclicTail`: includes first-run head from last-run tail.
- `piece_inverse`: a containing affine interval image yields `phiInv`.
- `imageLookup_sound`, `imageLookup_complete`, `imageLookup_eq_phiInv`:
  a sorted, covering, sound list index answers the inverse correctly.
- `extractHead_correct`: composes interval lookup with head-from-tail.
- `unmirror_tail`: the `.ri4` `(n-1)-sample` decoding identity.

The file records correspondence to the extractor's `(dest,start,len)`
interval images, half-open containment, prior-run tail, cyclic first head,
and `start + (query-dest)` result.

**Scope of optional target (c): list-function semantics proved.** This is
not a proof of the compiled binary-search loop or its O(log r) runtime.
The interval certificate `PieceSound`, image coverage, and sortedness are
explicit hypotheses. Deriving them from the serialized TeraLCP table,
validating machine arithmetic, proving the C++ decoder/search refinement,
and proving sorting cost remain separate bridges, documented in HANDOFF.
There are no blocked lemmas disguised by additional axioms or proof holes.

## Gate results

| Gate | Result |
|---|---|
| `lake build Sxgc SxgcBuild SxgcBounds SxgcPhi` | GREEN, all four libraries |
| `lake build` | GREEN, default executable |
| `lake env lean --run Main.lean` | 511 pass / 0 fail; BIT 1B GREEN |
| `lake env lean PhiEval.lean` | 17 texts, 381 positions; 0 Phi/inverse and run-boundary mismatches |
| Interval-index eval | 15 texts; 0 mismatches |
| `lake env lean PhiAxioms.lean` | All 19 new theorems audited; only permitted axioms |
| `lake env lean BuildEval.lean` | `true`, `true`, `true`, `false`, unchanged |
| `lake env lean BoundsEval.lean` | All recorded gates GREEN, unchanged |
| Rust slim vs executable Lean reference | 8/8 full witness-list equality |
| Slim aggregates vs committed chain baseline | 8/8 byte identity |
| Final slim LF telemetry | 0 walks / 0 steps on all eight texts |
| Protected files / existing evals | Byte-identical to HEAD |
| `git diff --check` / Python harness syntax | PASS |

The Phi battery includes empty/singleton indexed texts, small duplicate and
permuted texts, period-1 at length 100, period-2, and modular cycles. Expected
Phi edges come directly from adjacent entries of the brute SA cycle,
independently of the new rank-based functions.

[PhiAxioms.lean](PhiAxioms.lean) exhaustively enumerates every theorem declared
in the new module. The audit contains only `propext`, `Classical.choice`, and
`Quot.sound`; no `sorryAx` or evaluator axioms. No new use of native_decide.

Protected byte-identity audit: Sxgc.lean, SxgcBuild.lean, SxgcBounds.lean,
BuildEval.lean, BoundsEval.lean, Main.lean. The four pre-existing Sxgc proof
holes and conditional construction capstone remain unchanged.

## Differential evidence and reproduction

See [WELD_DIFFERENTIAL_REPORT.md](WELD_DIFFERENTIAL_REPORT.md) for the per-text
table, pinned inputs, exact coordinate adapter, artifact pipeline, fixture
preconditions, and limits. [weld-results.json](weld-results.json) retains all
raw and normalized witness lists and SHA-256 fingerprints.

From `lean/`:

```sh
lake build Sxgc SxgcBuild SxgcBounds SxgcPhi
lake build
lake env lean --run Main.lean
lake env lean PhiEval.lean
lake env lean PhiAxioms.lean
lake env lean BuildEval.lean
lake env lean BoundsEval.lean
```

Logs: `/tmp/laneT/{all-libs,default-build,main,phi-eval,phi-axioms,build-eval,bounds-eval,weld-run}.log`.
Existing dependency linter warnings are unchanged; the new proof module builds
without warnings. The requested contact_supervisor tool was absent from the
enabled tool catalog; no supervisor messages were sent.

These are measured local acceptance gates, not an assertion that an external
reviewer has approved the work or that compiled construction correctness has
been formally established for every input.
