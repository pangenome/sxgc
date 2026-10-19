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
xsa build -i pile.txt -s /scratch/pile  # one command: corpus in, one container out
xsa mems  --sxi hprc.sxi --reads r.fq.gz -j 32    # streaming MEMs: (name, offset, strand)
xsa serve --sxi hprc.sxi                # HTTP: /query /ms /batch /stats
```

`xsa build` is ONE entry and one journal. It reads the corpus through the
machine below (native AGC reader on the pangenome route — no materialized text,
no pipes, nothing text-sized on any disk), runs the χ sweep, an in-flight
sampled ground-truth audit against the corpus, and publishes one checksummed
`.sxi` container (RLBWT + run table + head/tail samples + anchors +
δ-compressed χ). Publication is fail-closed: no `.sxi` is written unless every
internal gate passes, and every journal binds the tool manifest, per-tool hashes
and the output SHA, so desync is impossible to miss. `--plan-only` prints the
surveyed, modelled build plan and exits.

## THE MACHINE (what `xsa build` runs, and why it is shaped this way)

The construction is a **byte-radix bucketing machine**, not a merge. A byte-1
bin's rows *are* a contiguous slice of the global BWT, so concatenating the 256
bins in byte order **is** the BWT: there is no global order to consult, no merge
walk, and no per-suffix position column anywhere in the product path
(`full_SA=0 persistent_positions=0` on every run).

1. **The multi-pass reader.** Pass 1 counts bytes per corpus chunk per bin
   (64-way parallel, one full pass); those counts are enough to prefix-sum every
   (bin, chunk) region, so pass 2 — the fill — is a **parallel scatter**: each
   thread writes its own chunk's positions into its own regions, no lock and no
   shared cursor, and concatenated in chunk order the regions hold each bin's
   positions in ascending position order (asserted, not assumed). This scatter is
   what killed the fill term: **213.006 s → 4.045 s at 10 GB/6 windows (52.7x,
   14.8 GB/s; 21.1 GB/s on a quiet box)**.
2. **Rolling window FILES.** Positions live in bounded rolling files in the
   scratch tree (a 4 KiB header + one 8 B arena word per admitted slot) — never
   in a corpus-sized arena. The fill writes them through **buffered `pwrite`**
   (per-(thread, bin) append buffers, water-filled to ~128 KB per stream and
   preallocated extents) and the build reads each bin's slice back with ONE
   sequential bulk `pread`. The write path used to be a `MAP_SHARED` mapping,
   which charged the whole window against the phase's `RLIMIT_AS` and died live
   at 10 GB on a 44.6 GB mapping (`sol10.log`); a window file of ANY size now
   charges ZERO bytes of address space, and the default `--bin-live-cap` is
   DERIVED from `--memory-gb` so the corpus cache + the fill staging + the arena
   pair always fit the phase cap with 10% headroom
   (`tools/binbwt/RESULTS_PWRITE_FILL.md`). Each file carries a completion word:
   missing, truncated, stale or corrupt ⇒ **hard refusal, never a silent
   rebuild** (all five sabotage modes are exercised by the oracle).
3. **Target-size recursive bins.** Bins subdivide by their next byte until every
   leaf is ≤ the target size, so **the tree's depth is an OUTPUT of the data**,
   not a parameter: a bucket whose rows keep sharing prefixes keeps being divided
   (one cheap coded symbol per row) and only a small node becomes a leaf — an
   LCP-carrying comparison sort from the already-certified depth. The default
   target is 1M rows, measured (`tools/binbwt/RESULTS_AUTO_RADIX.md`): the leaf
   sort costs `log2(m)` LCE walks per row, a bucketing level costs one symbol per
   row, and the exchange rate runs steeply toward small leaves. The depth cap
   survives only as a fail-loud backstop and journals every leaf it is forced to
   take over target.
4. **The ordered stitch IS the merge.** One sequential pass publishes the bins in
   byte order into the unchanged band/piece protocol: coalesce runs, number them,
   close 65536-run bands, hand each band to a writer thread. The row's position,
   BWT symbol and capture-target flag already travel in the 8-byte arena word, so
   the stitch never reads the corpus at random and never hashes a row.
5. **The decomposed finish.** `pile_finish` consumes those pieces CONCURRENTLY,
   flow-controlled through the piece window, verifies (χ + sampled witnesses),
   publishes the lite container and deletes the rlebwt. Its own work is
   **118.9 s/GB** after decomposition (211.8 s/GB before).

`--legacy-merge` still runs the pre-bin route (chunk front end → k-way merge
tree → adopt finish) for A/B; it is not the document. `--v2-pipeline` is accepted
and ignored: the bin machine *is* the default pipeline now.

### The measured pile arithmetic (1.3079 TB, 5 shards)

| term | measured anchor | pile |
|---|---|---|
| count sweep | 0.14-0.16 s per 10 GB pass, 64-way | minutes |
| fill (scatter) | 14.8-21.1 GB/s (was 282 MB/s serial) | **~0.5 h** (was 754 h) |
| build (bins, parallel) | 340-400 s per 10 GB at 96t | **~12.9 h** |
| stitch + finish | 118.9 s/GB (finish own work) | **43.3 h** |
| **end to end** | | **~57 h, finish-dominated** |

The launch limiter is the finish term and the per-shard arena capacity — a single
pile-scale bin (space, 0.166n = 2.17e11 rows) is indivisible and must be sorted
in one arena pair — not the fill (removed this week) and not the merge (retired).
`xsa build --plan-only` renders this honestly: window-file bytes on disc, one
build group of RAM, the target-size tree's leaves and emergent depth, and the
finish term, with the fit verdict for the machine it is run on.

## THE LAW, and D2 with the rolling amendment

**THE LAW: no O(n) random walks, ever — and the corpus file is the only text.**
Every text read goes through `CorpusText` windows (a bounded mmap view plus a
page pool); nothing text-sized is ever written: no T‖T, no `pftext`, no `m2.bin`,
no side copies, no sorted text.

**D2: NO PERSISTENT POSITION COLUMNS.** No array of 64-, 41- or 32-bit integers
with one entry per suffix, ever, at any phase — that is what every 4-8x blowup in
this project's history was (SA, PLCP on doubled text, WM = merged SA, order
vectors). **The rolling amendment (operator-directed):** suffix positions MAY
materialize *transiently*, in bounded rolling segments — the window files and the
per-window arena/scratch pair — during the count/scatter/bucket passes. The
implementation must enforce a numeric slot cap, release each segment before the
next, and journal the high-water mark; it does: `peak_live_slots`,
`peak_live_bytes`, `peak_window_file_bytes`, `window_files`, `window_loads`,
`win_file_write_bytes`, `win_file_read_bytes` are printed on every run and the
oracle asserts the bounds (`peak_live_slots <= 2*max(cap,n)`,
`peak_window_file_bytes <= credit*cap*8`).

## Measured, from scratch, from zero (no inherited artifacts)

| Corpus | n | χ (canonical) | from-scratch build |
|---|---:|---:|---|
| yeast235 (235 strains) | 3.34 Gbp | 85,404,336 | published 3× by 3 independent toolsets, byte-identical |
| 10-human subset | 30.2 Gbp | 1,627,067,257 | A/B double-build, byte-identical; peak RSS 134 GB |
| HPRC v2 466 haplotypes | 1.44 Tbp | *in flight* (historical pilot-frame value: 2,249,968,075) | running |

Output format: ropebwt3-compatible MEM tables (parity verified against the
reference tool built from source), name+offset annotation via the boundary
array, DNA/text modes with reverse-complement handling.

**Verification pyramid** (every change lands through it): a tiny brute-force
oracle over 134 adversarial fixtures in two modes × three thread counts × three
live caps (36,180 cases, exact brute SA, NUL/0x05/0x1e-wrap/periodic/nested/
non-identity-remap), a window-mode pass (12,864 cases: credit, file reuse,
build groups, RAM vs disc arms), the five crash-refusal modes, an adaptive-depth
pass for the target-size recursion (4,016 cases: the target bounds every leaf and
the tree really does go deep), and byte-identity + χ gates at 100 MB and 1 GB
(`29716349`, `283296933`) against the banked containers, single-window and
windowed.

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

- `xsa/` — the tool (Rust): build, mems, query, serve, stats; `cargo install`-able.
  `xsa/src/plan.rs` holds the versioned, gate-measured model constants;
  `xsa/src/chain.rs` is the one entry (bin pipeline by default).
- `bit6/` — the C++ stage tools: `binbwt_slice.h` (the bin machine: count →
  scatter → window files → target-size recursion → stitch), the stage binaries,
  the gate logs and the acceptance reports.
- `bit6/sxi_logs/pile-builder-v2/` — the pile builder's design ledger:
  `PILE_VISION_V1.md` (doctrine D1-D6 + the bin-era addendum + the PFP-radix
  domain map), the reports it supersedes.
- `tools/binbwt/` — the oracle, the gates, and the measurement reports
  (`RESULTS_BIN_DEPTH.md`, `RESULTS_PARALLEL_FILL.md`,
  `RESULTS_FINISH_DECOMP.md`, `RESULTS_AUTO_RADIX.md`).
- `lean/` — the formalization (Sxgc, SxgcBuild, SxgcBounds, SxgcPhi, SxgcNodup,
  SxgcRunEdge, SxgcSeam, LowerBound, LM) and the research plans
- `docs/` — the LM-over-χ backend spec (Emender-targeted, generic design)
- `RESEARCH.md` — the full working ledger: every gate, every correction
- `paper/` — the manuscript (sources; the PDF is a build artifact)

## History

Developed against HPRC v2 (466 haplotypes, AGC archive) as the rehearsal for
HPRC v3. The pilot-era chain (grlBWT, TeraLCP, `.ri4`/`.sA` artifacts) and the
comparison-merge chain that replaced it are retained as the historical record —
the merge was retired by measurement (13-20 TiB of position storage at pile
scale), not by preference. The product path is `xsa build` end to end.
License notes: r-pfbwt and pfp++ carry our upstreamable patches (AGC reader
and tail emission) — forks/PRs under the pangenome org; TeraTools MIT;
grlBWT GPL-3 (retired from the product path).
