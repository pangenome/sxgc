# forks-extract journal — the χ fork-matrix extractor (paper two)

Task: `xsa forks --jsonl <sxi> [--n N] [-o out.jsonl]` — STRING × (WITNESS, CLASS)
binary presence matrix; gates (a) yeast full, (b) 466 prefix smoke, (c) GWAS smoke.
Everything below was produced in this worktree, journal dir `bit6/sxi_logs/forks-extract/`.
Nothing is staged; retained artifacts were read only.

## Semantics (ratified, resolved against the artifact)

The audit (../forks/REPORT.md) found three blockers. The ratified presence
semantics dissolve them; the artifact reality settled the remaining
coordinate questions empirically:

1. **The artifact's BWT is over the FORWARD cyclic stream.** Verified on
   val.sxi: its run table equals the forward-BWT runs (3950/3950, char
   sequence identical); a reversed-text BWT gives 3967 runs and does not
   match. The forward-cyclic FM scan reproduces the artifact's chi EXACTLY
   (3398 = 3398, set equality); the reversed-convention scan does not (3405).
   Witness values are x = (n - SA) mod n (0 ≡ virtual end) — the same mapping
   witness.rs and the audit's run-edge checks use; every chi value maps to a
   forward run-edge row (0 misses on val.sxi; the extractor's hard
   `matched == chi-count` invariant also enforces it at full scale).
2. **Per witness**: x resolves to the run-boundary row it sits on — the row
   whose ssa (run-head SA sample) or ssa_t (mirrored run-tail SA sample)
   maps to x. The run(s) whose interval witnesses it are the runs meeting at
   that boundary (the witness row's own run and its boundary neighbour; a
   singleton run also contributes the two outer neighbours).
3. **Occurrences** = the rows of those runs. **Classes** = the DISTINCT
   next-symbol (BWT) characters realized among those occurrences — a row's
   BWT char lives at stream position (SA-1) mod n. Separated runs of the same
   character merge into one class (audit blocker 3). **Strings** = the
   collection members containing each occurrence's branch-character
   position; a separator position attributes to the record it terminates.
   Multi-label presence is kept (audit blocker 2): a string may realize
   several classes at one witness; nothing is collapsed. The witness's own
   event char is S[(n-x-1) mod n] = the BWT char of its row.

## Implementation

`xsa/src/forks.rs` (new; dispatch + usage in `main.rs`):
- dual-format: SXI1 (raw run table, packed mirrored ssa_t, raw ssa) and
  SXI2 v3+ (Huffman runs, compact φ edges for ssa/ssa_t);
- χ: SXI1 delta varints with a true --n early stop, SXI2 Elias–Fano; bitmap
  membership for small n, sorted vector + max filter for large n;
- matcher: per-run head/tail candidates against χ, threaded; singleton rows
  folded; hard failure if matched ≠ chi count;
- **φ-domain fast path** for row positions: φ domains are built from
  ssa/ssa_t alone — u = tail SA of a run, v = head SA of the next run, u
  radix-sorted for SXI1, already sorted for SXI2 — and φ(p) = v + (p-u) mod n
  is valid when the nonzero character counts have gcd 1 (criterion C, the
  same theorem the SXI2 writer's escape check relies on). Both yeast235 and
  hprc.sxi have gcd 1 (verified at runtime). Every run's chain is
  **endpoint-self-checked**: L steps from ssa must land exactly on ssa_t.
  The windowed LF walk (text-free, exits at run-head samples) remains as the
  fallback and is cross-checked against the fast path on 16 touched runs;
- strings via the embedded names TSV; per-run segments sorted+deduped in
  per-thread arenas; JSONL emission in text order (virtual end last).

`SOURCES.sha256.json` refreshed via `tools/package_xsa_sources.py` (the
build gate rejects modified sources otherwise). Side effect: the packager
re-synced 4 runtime snapshot files (`runtime/bit6/pfp_ds_vendor/pfp/dictionary.hpp`,
`seam_repair.hpp`, `sxi_pipeline.py`, new `sa_workspace.hpp`) that were
stale against the repo's own committed `bit6/` sources; no upstream source
was edited. Nothing is staged.

## Validation (small scale, independent oracle)

`forks_oracle.py` recomputes the entire matrix from corpus bytes alone
(forward cyclic SA/BWT/runs via prefix doubling), checks the artifact's own
ssa/ssa_t samples against the corpus SA, rebuilds every witness's records
and diffs the emitted JSONL record for record:

- val.fa (6 records, 23,966 B): **MATCH** — 3398 witnesses, 7101 records;
  samples OK over 3950 runs; 5971 multi-string classes; 7472 cross-class
  string overlaps (multi-label present and kept).
- val2.fa (5 records with tandem satellites, 8,605 B): **MATCH** — 1095
  witnesses; samples OK over 1307 runs.
- --n: witness order == sorted first-N chi; full order == full sorted chi.

## Gate (a) — YEAST FULL: PASS

`xsa forks --jsonl /mnt/nvme3n1/erikg/sxi2-v5/yeast235.sxi2 -o yeast.full.forks.jsonl`

- **χ = 85,404,336 witnesses matched exactly** (the hard invariant held:
  105,339,558 run-edge slots folded to exactly 85,404,336).
- **Every witness emits ≥ 2 classes**: 182,032,143 records over 85,404,336
  witnesses (2.13/witness), 6,465,022,491 string slots, 41,664,977,207 B
  output. 89,159,823 distinct runs witness the events; 2,950,587,688
  occurrence rows resolved (φ fast path, endpoint-checked, cross-checked
  vs the LF walk).
- wall 457.3 s total (validation 22.9 + tables 25.4 + χ 2.1 + match 7.2 +
  resolve 35.2 + emit 360.3); 186,769 witnesses/s end-to-end.
- **External witness-order check** (`witness_order_check.py`): all
  85,404,336 emitted witnesses equal the sorted χ stream exactly, each
  once, text order — PASS.
- **100-witness direct-text spot check** (`spot_check.py`): PASS. The
  checker independently decodes member 8 (numpy, a third φ
  implementation), reads run characters from the corpus bytes, recovers run
  extents by walking φ from ssa to ssa_t, and reads every occurrence's
  branch position from the corpus: 100 witnesses, 6,547 occurrence rows,
  char + record verified per row, emitted string sets equal — 0 failures.

## Gate (b) — 466 PREFIX SMOKE: PASS (measured)

`xsa forks --jsonl /home/erikg/sxgc-466/milestone/hprc.sxi --n 10000000 -o hprc.n10M.forks.jsonl`
(52 GB artifact, read-only; rerun under /usr/bin/time -v)

- **10,000,000 witnesses emitted exactly** (10,832,713 slots folded;
  hard invariant held), 20,251,533 records, 10,387,126,788 string slots,
  **60,443,628,959 B output**; byte-identical across two runs.
- **External witness-order check**: all 10M emitted witnesses equal the
  artifact χ stream in text order (virtual end 0 last) — PASS.
- Phases: validate 244.4 s (full 52 GB CRC + semantics) + tables 361.5 s
  (incl. radix sort of 2.74e9 φ domains) + χ (--n early stop) 0.1 s +
  matcher 3.3 s (all 2,739,737,285 runs scanned) + **resolution 252.7 s
  for 11,682,900,583 rows (46.3 M rows/s)** + emission 199.5 s (303 MB/s).
- **Wall 18:06.6; peak RSS 186 GB** (budget 400 GB).
- Throughput: 9,383 witnesses/s end-to-end (fixed costs included);
  sustained extraction rate ≈ 63 µs/witness scalable work.
- **Projected full-466** (2,250,211,129 witnesses): resolution ≈ 8.4 h
  (1.4e12 rows at 46.3 M/s) + emission ≈ 12.5 h (≈13.6 TB at 303 MB/s) +
  χ decode ≈ 30 s + fixed ≈ 10 min ⇒ **≈ 21 hours** wall; output ≈ 13.6 TB.
  HONEST LIMIT: the current in-RAM per-run string-set arena needs ΣL×4B ≈
  5.6 TB at full scale — a full-466 run requires a batched/streamed
  emission redesign; all other structures fit (<210 GB). The 21 h/13.6 TB
  projection assumes that redesign preserves the measured per-row and
  per-byte rates.

## Gate (c) — GWAS SMOKE: PASS

`gwas_smoke.py` (pure python + stdlib; p = erfc(sqrt(chi2/2))) over the full
yeast matrix: panel of 5,594 informative columns (≥3 of 100 study strings,
sampled by random seeks from the 41.7 GB matrix), 20 causal loci with
distinct class string-sets:

- **Empirical null** (permuted phenotypes): median p 0.40, p<0.05 3.2%
  (nominal 5%), Bonferroni α = 8.9e-6 with 0/5594 null hits.
- **Single-locus power** (association injected at one locus, P(case) =
  0.75 in class / 0.25 out; 20 loci × 5 replicates): **61/100
  Bonferroni-significant; median causal rank 1/5594** (random expectation
  2797); best rank 1. Recovery is real and dramatic vs the null.
- Joint 20-locus simulation (independent signed effects): top-20
  enrichment up to ~20× chance (1.6/20 vs 0.07 expected), no Bonferroni
  detections at n=100 strings — the honest small-n power limit, with the
  local-redundancy/LD behaviour the vision document's caveats predict
  (near-twin columns: a few per locus; saturated joint phenotypes are
  tracked by compound columns rather than single loci).

## Artifacts in this directory

- `yeast.full.forks.jsonl` (41.7 GB) + `yeast.full.stderr` — gate (a) output.
- `hprc.n10M.forks.jsonl` (60.4 GB) + `hprc.n10M.rerun.stderr` — gate (b).
- `yeast.chi.u64`, `hprc.chi.u64` (18 GB) — exported χ streams for the
  external checks; `*.pid` for the finished background runs.
- val/val2 corpora + SXI fixtures, oracle/checker/gwas scripts, the toy
  semantics explorations (`toy_*.py`, `forward_explore.py`).

No long-running processes remain. All changes are unstaged.
