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
cargo install xsa                                # complete product, all stages vendored
xsa build --tape -i pile.txt -s /scratch/pile \
           --docs-map pilemap/ --trie-depth 8 --trie-min-support 1000
                                                  # ONE command: corpus in, one container out;
                                                  # trie + document structure embedded IN-BUILD
xsa query  pile.sxi2 --count "the "               # instant counts, straight from mmap
xsa consult --container pile.sxi2 --windows w.jsonl --output rows.tsv
                                                  # bounded batch contexts (the Emender surface)
xsa branch-of pile.sxi2 "the qu" --batch         # trie-backed continuation branches
xsa search pile.sxi2 --pattern " function " --corpus
                                                  # every occurrence, boundary law enforced,
                                                  # --exclude-source to leave a document out
xsa mem    pile.sxi2 "the quick brown fox"          # maximal exact matches (widths + positions)
```

### One container format, and nothing else

FORWARD-ONLY. We are the only consumer of this container family, so exactly ONE
shape is supported: what `xsa build` emits today, **SXI2 v10** — the
block-compressed run stream (m1 with its m12/m13 directories), the locate
anchors (m11), χ (m5), the document starts (**m15**) and the context trie
(**m20**). Every command refuses any other file **by name**, prints the rebuild
instruction, and there is no second reader, no degraded mode and no
`--legacy-*` escape hatch:

```
$ xsa query /mnt/old/pile.sxi2 --count "the "
xsa: error: /mnt/old/pile.sxi2: SXI2 v6 container (an older shape: below the supported one)
  this build serves exactly ONE container format: SXI2 v10 with m15 (document starts) and m20 (context trie)
  rebuild with `xsa build`.
```

Artifacts in older shapes (`.ri4`, SXI1, SXI2 v2..v9) are **unsupported**: they
are not readable, not convertible in place and not repaired by any flag. Rebuild
them from the corpus with `xsa build --docs-map …`. The banked SHAs in git stay
as historical records of what was measured; they do not imply the ability to read
those files. The family may still gain members over time — that is additive, and
needs no reader change.

`xsa build` is ONE entry and one journal. It reads the corpus through the
machine below (native AGC reader on the pangenome route — no materialized text,
no pipes, nothing text-sized on any disk), runs the χ sweep, an in-flight
sampled ground-truth audit against the corpus, and publishes one checksummed
`.sxi` container (RLBWT + run table + head/tail samples + anchors +
δ-compressed χ). Publication is fail-closed: no `.sxi` is written unless every
internal gate passes, and every journal binds the tool manifest, per-tool hashes
and the output SHA, so desync is impossible to miss. `--plan-only` prints the
surveyed, modelled build plan and exits.

## Memory operations over the container

The document map (**m15**: docstarts, rank/select over delta-varint gaps + sampled
absolutes) and the context trie (**m20**: the top of the suffix tree,
boundary-aware, built in-build from the sorter's own band stream at ~1% of
build time) are container MEMBERS — one file answers the whole operation set,
no sidecars:

| operation | command | measured |
|---|---|---|
| count a pattern | `xsa query <c> --count P` | instant from mmap; 70,412,767 for `"the "` at 10 GB |
| contexts at positions | `xsa consult` | ~5,000 positions/s, ≤8 GB RSS hard cap, deterministic shas, kill-resumable |
| continuation branches of a context | `xsa branch-of [--batch]` | 4.7 µs/query; ~148k/s per core batched; 59.5% coverage at depth 8 / MS 1000, 0.035n bytes |
| every occurrence, law-enforced | `xsa search` | crossings excluded by default; `--exclude-source` width-1, verified both directions |
| maximal exact matches | `xsa mem` | interval widths + bounded positions |
| branch enumeration | `xsa branches` | 22k contexts/s |

**THE LAW at query time: no match crosses a document boundary.** True by
construction — the trie cuts at document ends, and search excludes any match
where `rank(s) ≠ rank(e−1)`; `--cross-doc` is the explicit opt-in and every
escape hatch is journaled. Boundaries come from the document map, never
inferred from content bytes. Honest bound: `locate` (position of a single
occurrence) is a ~40 ms LF-walk, so per-position aggregation over very common
patterns routes through the stored structures (trie, consult) instead.

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
5. **The decomposed finish.** `xsa_finish` consumes those pieces CONCURRENTLY,
   flow-controlled through the piece window, verifies (χ + sampled witnesses),
   publishes the lite container and deletes the rlebwt. Its own work is
   **118.9 s/GB** after decomposition (211.8 s/GB before).

There is ONE route. The pre-bin chunk+merge front end is gone (its flags
`--legacy-merge`, `--chunks`, `--chunk-mb`, `--fanin`, `--kway` are refused by
name), and `--v2-pipeline` is accepted and ignored: the bin machine *is* the
pipeline, and `--tape` selects its producer, not a different route.

### The measured pile arithmetic — tape era (1.3079 TB, N = 1,307,910,802,540)

The tape route deleted the finish term and the merge. Measured end to end on
the **100 GB pile head** (96 threads): wall **12:35:10**, 102 waves, peak
scratch 78.9 GB = **0.789n**, χ = 24,185,339,204, container 0.632n. The sorter
(band production) now dominates; the finish is a streaming consumer that rides
along and ends with the tape. The full pile is ~13.1× the head: **~7 days** at
a 200–300 GB scratch budget, with point-mass classes (whitespace: the 16-space
run is 0.76% of the head) handled by the one-wave borrow rule — capped,
journalled, and proven byte-identical to the uncapped build.

Backpressure, not kill: `--scratch-budget-gb` pauses the producer (SIGSTOP/
SIGCONT on the process group) when live scratch exceeds the budget; the build
ends when it ends, fails, or a human stops it — no timeouts, no watchdogs,
and `checkpoint.journal` + spool-presence resume make an interrupt a work-
avoiding restart, not a rerun.

`xsa build --plan-only` renders this honestly on any machine: window-file
bytes on disc, one build group of RAM, the waves, and the fit verdict — and
refuses with the shortfall when it does not fit (`--skip-fit-check` to
override).

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
| HPRC v2 466 haplotypes | 1.44 Tbp | 2,282,463,145 | done, certified, banked |
| Pile head (first 100 GB) | 100 GB | 24,185,339,204 | 12:35:10, peak scratch 0.789n |

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

- `xsa/` — the tool (Rust): build, query, search, consult, seeds, mem, branches,
  stats; `cargo install`-able.
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
- repo-root serving & ops docs: REPRODUCE.md, SERVING.md, SXI_FORMAT.md,
  CONSULT.md, MEMORY_OPS.md, CONTEXT_TRIE.md, CORPUS_V2.md, RESUME.md,
  HEARTBEAT.md, and the RESULTS_*.md ledger — every number in this README
  traces to one of them
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
