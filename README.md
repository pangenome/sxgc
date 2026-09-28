# sxgc — χ marks the fork

**Suffixient-array (χ) indexes at pangenome scale: built from scratch in one
command, proven correct in Lean, and bounded from below for the first time.**

Almost every byte of a corpus is *forced* — given its left context, only one
continuation occurs. The freedom is a sparse skeleton of *forks*, and χ (the
minimum suffixient set, Cenzato–Depuydt–Gagie–Kim–Manzini–Olivares–Prezza
2024) measures it. Measured on this project's corpora: χ/n = 2.6% (yeast235,
3.34 Gbp), 5.4% (10-human subset, 30.2 Gbp), 0.16% (HPRC v2 466 haplotypes,
1.44 Tbp). This project makes χ two-sided: it upper-bounds what an index
*must* store, and — proven here for the first time — lower-bounds what any
position-answering system *must* consult.

## What you can do today

```sh
cargo install xsa                       # complete product, all stages vendored
xsa build --agc hprc.agc -o hprc.sxi    # one command, archive in, one file out
xsa mems  --sxi hprc.sxi --reads r.fq.gz -j 32    # streaming MEMs: (name, offset, strand)
xsa serve --sxi hprc.sxi                # HTTP: /query /ms /batch /stats
```

`xsa build` reads the AGC archive *natively* (the parse stage carries an
in-process AGC reader — no materialized text, no pipes, nothing text-sized on
any disk), then runs the front-end, seam repair, aggregates, the χ sweep, an
in-flight sampled ground-truth audit against the archive, and publishes one
checksummed `.sxi` container (RLBWT + run table + head/tail samples +
anchors + δ-compressed χ). Publication is fail-closed: no `.sxi` is written
unless every internal gate passes. Every build's journal binds the tool
manifest, per-tool hashes, and output SHA — desync is impossible to miss.

## Measured, from scratch, from zero (no inherited artifacts)

| Corpus | n | χ (canonical) | from-scratch build |
|---|---:|---:|---|
| yeast235 (235 strains) | 3.34 Gbp | 85,404,336 | published 3× by 3 independent toolsets, byte-identical |
| 10-human subset | 30.2 Gbp | 1,627,067,257 | A/B double-build, byte-identical; peak RSS 134 GB |
| HPRC v2 466 haplotypes | 1.44 Tbp | *in flight* (historical pilot-frame value: 2,249,968,075) | running |

Output format: ropebwt3-compatible MEM tables (parity verified against the
reference tool built from source), name+offset annotation via the boundary
array, DNA/text modes with reverse-complement handling.

## Proven (Lean 4; zero axioms beyond propext / Classical.choice / Quot.sound)

- **The construction capstone**: a one-pass scan over the text's suffix-array
  stream emits a minimum suffixient set — proven subject to named,
  explicitly-tracked hypotheses (the no-duplicates pillar is discharged
  outright; the remainder is reduced to a single statement about run-edge
  domination, one of whose two factors is proven).
- **The continuation bridge**: any system that answers continuation queries
  with corpus positions must consult ≥ χ *distinct* positions. This is the
  formal warrant for "the index is the rulebook; the model is the policy"
  (see `docs/LM_CHI_BACKEND_SPEC.md` for the LM-facing design).
- **The first space lower bound for a pattern-matching class**: on the
  witness-perturbation family, any fixed-decoder bit-space index realizing
  correct answers needs `s + 1 ≥ χ·log₂(n/χ)/3` bits. No lower bound
  proportional to any repetitiveness measure existed before, for any
  pattern-matching operation class (verified survey in
  `lean/LOWER_BOUND_PLAN.md`).
- Bounds with explicit constants; the φ⁻¹ position-extraction algebra; the
  seam-repair theorems; determinism of the parse-space LCE machinery.
  Every statement links to its exact Lean source (commit-pinned URLs).

## The paper

`paper/` — *χ Marks the Fork: Suffixient Arrays and a Space Lower Bound for
Locating Patterns* (draft; the 466 headline number lands with the running
build). `paper/FACTS.md` maps every claim to its artifact.

## Repository map

- `xsa/` — the tool (Rust): build, mems, query, serve, stats; `cargo install`-able
- `bit6/` — the C++ stage tools, the pipeline, gate logs, acceptance reports
- `lean/` — the formalization (Sxgc, SxgcBuild, SxgcBounds, SxgcPhi, SxgcNodup,
  SxgcRunEdge, SxgcSeam, LowerBound, LM) and the research plans
- `docs/` — the LM-over-χ backend spec (Emender-targeted, generic design)
- `RESEARCH.md` — the full working ledger: every gate, every correction
- `paper/` — the manuscript (sources; the PDF is a build artifact)

## History

Developed against HPRC v2 (466 haplotypes, AGC archive) as the rehearsal for
HPRC v3. The pilot-era chain (grlBWT, TeraLCP, `.ri4`/`.sA` artifacts) is
retained as the historical record; the product path is `xsa build` end to end.
License notes: r-pfbwt and pfp++ carry our upstreamable patches (AGC reader
and tail emission) — forks/PRs under the pangenome org; TeraTools MIT;
grlBWT GPL-3 (retired from the product path).
