# SLIM pass-4 acceptance report — PASS

**G0, yeast, and full k10 passed.** Both full aggregates are byte-identical
to their baselines, both chi counts match, and NumPy verifies both witness
sets exactly. Every full-dump run head resolved directly with zero LF steps.
The complete k10 dump/comparison/sweep/set pipeline took **5809.321 s
(1h 36m 49s)** at **73.982 GB** peak RSS.

## Implementation and extraction

- `tools/extract_head_sa.py` reads CRA1 sequentially with bounded buffers,
  validates exact source size/count, and publishes raw LE u64 `saFirst`.
- `h10.head_sa`: **1,859,825,801 values**, **14,878,606,408 bytes**, extracted
  in **44.965 s**. The entire sidecar is byte-identical to its source column.
  1,025 spread mirrored-tail checks and 32 singleton-head checks passed;
  another 1,025 independent LF-decoded head/tail checks had zero mismatches.
- Slim `--head-sa FILE` maps exactly 8R bytes and resolves known run heads,
  tails, and previous tails directly. Absent the sidecar, the original LF
  resolver and optional exact cache remain available. LF tables are retained.
- The first k10 prefix gate exposed a pre-existing collection-LCP error:
  raw PFP LCE continued across shared newline terminators. The correction
  derives sparse string ends during existing dictionary/parse scans and
  clamps collection LCP before newline. It recovered exactly 865 k10 ends;
  all four fields matched on 10,000 leading plus 100,000 spread runs.
  No text scan, M, b_bwt, or w_wt is introduced.

## Gates and measured costs

| Gate | Result |
|---|---|
| G0, after collection correction | 8/8 texts, including duplicates-600k; sidecar and fallback aggregates both byte-identical to every baseline |
| Input failures | Wrong sidecar size and out-of-range SA rejected; extractor rejects wrong R, truncation and trailing data |
| Yeast aggregate | Byte-identical to `/tmp/laneY/yeast_pfp2.agg` |
| Yeast streamed sweep | chi = **85,404,240**; NumPy sorted arrays equal the oracle, with no duplicate entries |
| Yeast resolution | **34.839281 s**, versus supplied **272 s** reference; **0 LF steps** |
| Yeast dump | **502.60 s**, **3,492,948 KiB** peak (**3.577 GB**) |
| Yeast dump + comparison + sweep + set verification | **534.832 s** |
| K10 aggregate | Entire file byte-identical to `h10.walk.agg` |
| K10 resolution | **437.509169 s (7.292 min)**; **0 LF steps** across all 1,859,825,801 head queries |
| K10 dump | **5255 s (1h 27m 35s)**; **72,248,348 KiB** peak (**73.982 GB**) |
| K10 streamed sweep | chi = **1,627,063,183**; **213.928 s** monitored wall; **6,144 KiB** peak |
| K10 NumPy sorted-set equality | **PASS**, all **1,627,063,183** entries equal the oracle and are unique; **300.725 s** monitored wall |
| K10 complete pilot | **5809.321 s (1h 36m 49s)**, including dump, full cmp, sweep and NumPy verification; **73.982 GB** peak |

The earlier pass-3 cost document records a different yeast run at 329.067 s
resolve; 272 s is the task-supplied comparison. Timings share the machine
with other work and are not controlled single-tenant throughput experiments.
A three-repeat exact 110,000-run probe favored 64 LCE workers (median 0.184 s)
over 32 (0.223 s). All 18 thread configurations returned identical answers.

## 466 scope and provenance

The raw sidecar costs **21.920 GB** at R=2.74e9. The corrected conservative
projection is **213.867 GB with LF retained**, or **170.027 GB without LF**.
The no-LF variant remains unimplemented. The earlier 205.990/162.150 GB
pair assumed packed 41-bit heads and is not this implementation's cost.
Sparse collection-end metadata falls within the existing reserve.

These pilot sidecars deliberately come from the supplied aggregate artifacts.
The binding v5 end-state requires the mandatory front-end to emit head/tail
samples and anchors directly, with no separate TeraLCP/lcp_index/O(n) sample
walk. That provenance adds no separate generation pass, but sample storage
still counts. Front-end integration, the native v5 container, and a 466 build
are not validated here. Exact LCE verification remains proportional to the
matched prefix; no deterministic O(tau) claim is made.

The failed unclamped k10 attempt was stopped only in this lane's process
group after 759.857 s and retained for diagnosis. Successful pilot timing
excludes that failed attempt and the separately reported 44.965 s extraction
and diagnostic probes. Adding extraction to the successful pipeline costs
5854.286 s of sequential work; intervening re-gates are not folded into it.
No commits, staging, or interference with the Lean lane. All measured peaks
were below 150 GB. No full-pilot stages remain running.

Evidence: `bit6/gate_logs/slim/pass4/`; commands and raw timings are in the
phase logs. Large artifacts: `/tmp/laneQ/pass4/` and the supplied k10 directory.
Rebuild with `bash tools/build_slim_dump.sh /tmp/laneQ/pass4/slim_dump`
and `cargo build --release --manifest-path xsa/Cargo.toml`; run
`python3 tools/slim_pass4.py --stage G0`, `--stage G1`, then
`--stage K10 --threads 64`. Existing head sidecars are byte-checked before reuse.
