# Lean / Rust slim weld differential — 2026-09-27

**GREEN: 8/8 texts; zero witness disagreements; all aggregate files byte-identical.**
Base: `1cfef7d`, workspace `/tmp/sxgc-laneT`. No production source edits.

## Per-text results

Witness comparison is sorted-list equality (multiplicity preserved), stronger
than set equality. Rust counts below use the coordinate adapter described next.

| Text | Bytes excluding sentinel | BWT runs | PFP p | Phrases | Lean witnesses | Rust witnesses | Full agreement | Aggregate identity |
|---|---:|---:|---:|---:|---:|---:|---|---|
| duplicates-100 | 100 | 2 | 8 | 2 | 1 | 1 | YES | YES |
| alternating-60 | 60 | 3 | 7 | 27 | 2 | 2 | YES | YES |
| ascending-32 | 32 | 11 | 5 | 7 | 15 | 15 | YES | YES |
| descending-32 | 32 | 9 | 5 | 6 | 15 | 15 | YES | YES |
| banana-42 | 42 | 6 | 5 | 7 | 3 | 3 | YES | YES |
| random-dna-64 | 64 | 54 | 5 | 10 | 40 | 40 | YES | YES |
| prefix-mix-71 | 71 | 5 | 5 | 5 | 3 | 3 | YES | YES |
| duplicate-blocks-96 | 96 | 13 | 5 | 8 | 10 | 10 | YES | YES |

Exact Lean, raw Rust, and normalized Rust lists, input SHA-256 and aggregate
SHA-256 are recorded in [weld-results.json](weld-results.json).
All final slim runs logged `SLIM_RESOLVE walks=0 steps=0` and
`head_lf_steps=0`.

## Fixed semantic / coordinate contract

The file contains a single indexed string `S` followed by newline. The Lean
reference sets `T = reverse S`; thus `parseTriplesOf T` indexes `S ++ [0]`.
Production newline is Lean's sentinel zero; all other bytes are unchanged.

`ri4.n = |S| + 1 = |R|` is asserted for every case. Lean's construction emits
`|R| - SA`; committed `xsa/src/main.rs` explicitly sets `big_n = ri4.n + 1`
and emits `big_n - SA`. Therefore **Rust raw minus one = Lean coordinates**.
This adapter was fixed from source before the first comparison, not inferred
or fitted from results. Raw lists are retained. Example: duplicates-100 emits
Lean `[100]`, Rust raw `[101]`, and Rust normalized `[100]`.

For each exact input, [WeldReference.lean](WeldReference.lean) evaluates the
committed executable `parseTriplesOf` / `scan` machinery at phrase widths
2, 3 and 5 and requires all three lists to agree. The reference uses the
nonoverlapping `SxgcBuild.Parse` model; pfp++ uses overlapping phrases.
These parses need not have matching IDs/boundaries: they represent the same
indexed text and the construction theorem is independent of the parse.
This is a differential instrument for the implementation bridge, not a
formal proof of the overlap-coordinate conversion.

## Artifact chain and reproduction

[tools/lean_weld_gate.py](../tools/lean_weld_gate.py) implements the committed
`bit6/chi_rspace_battery_chain.sh` recipe: grlBWT → run-length files →
TeraLCP → committed teralcp_chi `.ri4` plus baseline CRA1 → pfp++ → slim
CRA1 → Rust `chi-rspace --stream-agg`. Head-SA fixtures are the baseline
CRA1 `saFirst` column, exactly the pass4 pilot sidecar convention. Fixture
creation uses the tiny baseline walk; the final slim runs use only the
sidecar and parse, with zero LF steps. This does not claim to differentially
test the separate Phi table decoder/extractor binary.

All input recipes (including random seed 20260927) are pinned in the script.
Duplicates-100 is `A` repeated 100 times; ascending/descending-32 are eight
symbols, each repeated four times. The compiled TeraLCP accepts at most 16
symbols, so the untested initial 32-symbol fixtures were replaced after
input rejection. No witness comparison ran on those rejected fixtures.

The slim loader has fixed window 10 and needs at least two phrases. The
chain's default p=100 produced one phrase on the tiny homopolymer. Supported
p values in the table were fixed by parse-length-only preflight, before
witness comparisons. All successful comparisons used these final values.
No witness or aggregate disagreement occurred; the harness stops immediately
if either does occur. The pre-existing oracle binary lacked `--agg-out`, so
both C++ programs and Rust were rebuilt from this workspace's committed code.

```sh
mkdir -p /tmp/laneT
bash tools/build_slim_dump.sh /tmp/laneT/slim_dump
bash tools/build_slim_dump.sh /tmp/laneT/teralcp_chi bit6/teralcp_chi.cpp
cargo build --release --manifest-path xsa/Cargo.toml
(cd lean && lake build SxgcBuild)
python3 tools/lean_weld_gate.py --work /tmp/laneT
```

Per-phase commands, stdout/stderr, `.ri4`, `.parse`, dictionary, `.head-sa`,
`.agg`, and raw `.rust.sA` artifacts remain under `/tmp/laneT/`. Final output:
`WELD GREEN: 8/8, zero disagreements`.

This small single-string battery is not a scale or multi-collection proof,
and does not discharge the conditional chi capstone's open O1–O4 obligations.
