# SLIM final pass-3 cost and acceptance evidence

G1, G0 revalidation, G2 measurement, and G3 passed in the requested order.
This establishes yeast end-to-end correctness with independently generated
real samples. It does not establish a 466 build or a deterministic O(tau)
query bound. The incompatible legacy parse-resolver mandate is retired by
the task's correction #6. No M, b_bwt, or w_wt was built in this pass.

All GB are decimal; KiB are 1,024 bytes. Logs: `bit6/gate_logs/slim/pass3/`.
Artifacts: `/tmp/laneQ/pass3/`. All measured process peaks are below 150 GB.

## Inputs and independent preparation

Yeast n=3,336,986,760; R=100,904,881; P=34,134,006;
virtual dictionary D=513,395,745 bytes; 3,159,456 distinct phrases.
The matched chain is `yp2t.lcp_index.lcp_index` with `yp2t.bwt.heads/len`.
`yeast_pfp2.txt` has one trailing newline and 9,901 internal `!` bytes;
the BWT has exactly one newline endmarker, so the sidecar is one row:
`s0\t0\t3336986759`. Splitting this chain at `!` would be wrong.

The existing `/tmp/grl_gate/teralcp_chi` production walk created
`yp2real.ri4` using that index, RLBWT, and text-derived sidecar, with witness
output directed to `/dev/null`. It used neither aggregate nor witness oracle
to generate samples. Header n/R match; all 100,904,881 packed samples are
below n, including first/middle/last; run characters and lengths match the
paired runs-only artifact byte for byte. Exact spot values are in `gates.log`.

Preparation: **3237.03 s**, **10,350,540 KiB** peak
(10.598953 GB). This O(n) walk is separately charged legacy
remediation for this pilot artifact, not the end-state architecture or a
slim parse-space sample construction. The binding provenance clarification
following correction #6 in `/home/erikg/sxgc/RESEARCH.md` requires the
mandatory PFP/RLBWT front-end build to emit RLBWT, head/tail SA samples,
and anchors together. End-state slim construction uses no TeraLCP,
no lcp_index, and no separate O(n) sample walk. That front-end integration
is not implemented or gated by this pass. `-t 32` was requested,
but the single-string fill phase has only one active worker. Both jobs ran
under owned-process watchers; no other lane's processes were touched.

## Full yeast build and sweep

Full slim construction used `--slim --resolve-ri4 --resolve-cache --dict-stream`, 32 threads,
and the actual real-sample ri4. The resulting `yeast.slim.agg` is byte-identical
to `/tmp/laneY/yeast_pfp2.agg`. Flat calibration used ri4 row offset 0 and
PFP text shift 10. Total: **1209.93 s**, **4,809,948 KiB** peak
(4.925387 GB).

| Phase | Wall seconds | Cumulative process high-water RSS, KiB |
|---|---:|---:|
| ri4-load | 1.053 | 1,677,312 |
| dict-read | 0.000 | 1,677,312 |
| dict-boundaries | 4.978 | 1,681,408 |
| parse+boundaries | 8.304 | 1,857,536 |
| parse-fingerprints | 0.188 | 1,945,600 |
| dict-fingerprints | 3.096 | 2,043,904 |
| lf-build | 1.822 | 3,228,892 |
| resolve-cache-build | 0.874 | 4,801,756 |
| flat-calibration | 29.381 | 4,801,756 |
| All chunks: resolve | 329.067 | 4,809,948 |
| All chunks: lce | 826.988 | 4,809,948 |
| All chunks: write | 3.633 | 4,809,948 |

Resolve/LCE/write phases are timed separately within each 65,536-run chunk,
then summed. They include worker creation/join overhead. RSS is the cumulative
process high-water mark sampled after each phase, not an isolated allocation
measurement. Build totals also include small allocation/close/report overheads.
These runs include measurement counters and share the machine with other work;
they are not controlled single-tenant throughput benchmarks.

| Sweep of the new slim aggregate | Wall seconds | Peak RSS KiB |
|---|---:|---:|
| Streamed aggregate | 12.12 | 4,096 |
| Resident aggregate | 114.05 | 6,022,540 |

Both sweeps returned **chi=85,404,240**. Their raw witness files are
byte-identical and NumPy sorted arrays equal the supplied
`chi_yeast_pfp2.sA` oracle (85,404,240 entries). The streamed sweep retains
four 512 KiB aggregate buffers, a 1 MiB character buffer, and a 1 MiB output
buffer, independent of aggregate and output sizes.

## Deterministic verification and measured query lengths

Across the complete yeast construction, **177,082,171 seed LCE
queries** performed **2,714,557,392
verified phrase positions**. Mean verification length is
**15.329365891 phrases per seed**, including seeds that
need no parse-level comparison; maximum is **22262
phrases**. Restricted to the **36,313,332 nonidentity parse
fingerprint calls**, the mean is **74.753740362**, with the same
maximum. A position counts once per paired-symbol comparison and includes
the first unequal boundary where present; these are logical verification
lengths, not raw memory-read counts (short probes may reread positions).
Identical indices need no verification. Each seed has at most one parse call.

Dictionary level: 192,895,305 nonidentity calls,
6,469,374,680 verified byte positions; mean
33.538269270, maximum 17549 bytes per counted call.

Full resolver counters (including startup flat calibration):
277,987,564 calls, 2,098,524,848 LF steps,
7.548988 steps/call, maximum 1439226;
anchor fallback 0, failed sentinel resolutions 0.
This call mean includes direct tail lookups and must not be confused with
the k10 run-head-only mean of 2,156 steps.

The uncached pilot attempt spent 259.065 s resolving its first 65,536-run
chunk versus 0.347 s in LCE. It was stopped before completing G1; its log
is retained as `yeast.dump.uncached-attempt.log`, not claimed as a completed
build. The successful run uses an optional bounded exact LF cache: at most
67,108,864 slots, 24 bytes/slot (1,610,612,736 bytes). Each slot stores a full
row key and exact SA value under a lock; collisions evict rather than match.
Entries are derived only from successful ri4-sample/anchor LF resolutions.
Each walk uses at most 4,096 checkpoints (64 KiB per active call), compacting
their spacing on longer walks. There is no dense n-sized map and no oracle
seed. This run recorded 64251049 cache hits.
The cache was differentially tested against explicit suffix arrays with
forced eviction, long walks, and concurrent access, then against the
uncached standalone path in G0. It improves repeated walks but supplies
no constant-time or worst-case query guarantee; v5 heads remain the adopted fix.

Hashes only propose an LCE. Every position in the proposed equal prefix,
plus the excluded boundary when present, is checked directly. A bad proposal
causes a fatal exit. Query verification is proportional to the matched length;
there is **no deterministic O(tau) bound**. Polynomial-hash search additionally
costs sampling/probe work; exact verification still pays for long repeats.

## Tau sensitivity

These 100,005 spread-run probes use positions from the now-validated slim
aggregate. They isolate LCE behavior and do not remeasure position resolution.
Every configuration agrees with all tested aggregate LCE values. Query wall
includes aggregate seeks/reads and is single-threaded; cache state is uncontrolled.

| Configuration | tau1 | tau2 | Fingerprints MB | Query s | Mean verified phrases/parse call | Max phrases | Whole probe peak KiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| class1 | 1 | 6 | 957.600 | 1.640 | 74.616960365 | 16233 | 1,617,920 |
| default | 3 | 41 | 191.199 | 1.647 | 74.616960365 | 16233 | 868,352 |
| default-stream | 3 | 41 | 191.199 | 8.067 | 74.616960365 | 16233 | 368,640 |
| class32 | 11 | 163 | 50.022 | 1.764 | 74.616960365 | 16233 | 731,136 |

The whole-prefix verification work is insensitive to tau; denser sampling
changes fingerprint memory and proposal cost. G3 separately ran tau1=2 on
all eight battery texts: all clean aggregates matched their baselines, and
all eight explicitly perturbed proposals were rejected with
`FATAL SLIM: fingerprint verification mismatch`. Tau1=2 alone does not
induce a collision; explicit fault injection is required for that gate.

## Conditional 466 projection, including adopted v5 head samples

Assume n466=1.403e12 and r466=2.74e9. Measured k10 inputs are
n10=30,151,407,545, P10=333,531,723, virtual D10=5,483,754,406,
and 37,705,212 dictionary phrases. Holding their densities fixed gives
P466=15.51984 billion, D466=255.169 GB, and 1.75449 billion dictionary
phrases. True growth is unknown. Phrase and run IDs still fit uint32 under
these assumptions. Sparse-boundary constants are measured yeast bytes per
phrase, with similar Elias-Fano width classes at k10; allocator/select
overhead at 466 is unmeasured.

Default tau1=ceil(8P/r)=46, tau2=ceil(8D/r)=746. The factor eight is
material; literal ceil(P/r),ceil(D/r) sampling needs more memory.
Dictionary text is accessed through a four-page, 256 KiB/thread pread cache,
not retained or mmap'ed. Reclaimable filesystem cache is excluded from RSS.
The yeast dictionary fits readily in filesystem cache after the build scans;
the projected 255 GB dictionary may not. Random-read throughput and cache
eviction at 466 are unmeasured, so these query times are not a 466 wall-time
prediction. Exactness is unaffected by eviction; throughput need not be.

| Current v4 structure | 466 GB |
|---|---:|
| parse | 62.079 |
| parse_sparse_boundaries | 18.740 |
| dict_sparse_boundaries | 2.171 |
| parse_fingerprints | 2.699 |
| dict_fingerprints | 2.736 |
| ri4_runs | 13.700 |
| ri4_starts | 21.920 |
| ri4_samples | 14.043 |
| LF_capacity_upper_bound | 43.840 |
| chunks_and_dict_caches | 0.019 |
| Subtotal | 181.947 |
| Unmeasured allocator/support reserve | 10.000 |
| Current v4 conditional peak | 191.947 |
| Optional bounded exact LF cache | 1.611 |
| v4 with that cache enabled | 193.558 |
| Additional packed v5 head samples (41 bits/run) | 14.043 |
| v5 with current LF tables retained | 205.990 |
| Future v5 direct-head/tail mode omitting LF tables | 162.150 |

V5 direct-boundary lookups do not need this optional cache, so the v5 rows
add head samples to the uncached model. Enabling it anyway adds 1.611 GB;
worker checkpoint stacks and other stack overhead fall within the explicit
10 GB reserve. Its yeast speedup is not extrapolated to 466.

The existing LF capacity bound is 16r bytes (up to 8r for uint32 character-run
vector capacities plus 8r for prefix sums). The conservative v5 model retains
both LF and row-start arrays; the optional no-LF model subtracts only LF.
The latter is an unimplemented optimization, not a measured win. Native v5
anchors and header details are not frozen; the 10 GB reserve must cover them
and other omitted overhead, or the projection increases. No v5 reader/writer
or 466 build was implemented or tested in this lane.

The measured **k10 v4 baseline is 2,156 LF steps/run-head on average,
maximum 933,995**. Holding that distribution fixed at 466 implies
**5.90744e12 head-resolution LF steps**, not a measured wall-time prediction.
The adopted v5 fix stores head samples alongside tails: all required seed
positions (head, tail, previous tail) become **O(1) direct lookups**. LF
remains available for arbitrary rows if retained, but it is not needed for
those sampled boundaries. The mandatory front-end must emit those samples
and anchors from its internal build structures; its integration cost is
not measured here. A separate TeraLCP/O(n) walk is excluded from the
end-state architecture, as clarified after correction #6.

The conservative v5 model exceeds 200 GB by 5.990 GB. Current v4 at
191.947 GB does not include the head-sample fix and must not be advertised
as its memory cost. Literal sampling gives 228.921
GB before adding v5 heads; retaining dictionary text gives
447.116 GB before v5 heads. Both alternatives
also exclude the optional LF memo cache. All are conditional
models, not measured guarantees. The 150 GB operational cap was respected
by these yeast/battery runs; none of these 466 estimates promises that cap.

CRA1 at 466 is 87.68 GB on disk; the raw witness output is about 18 GB.
Front-end creation of P/D plus head/tail samples and anchors, staging disk, and
kernel cache are outside the slim-process projection. The streamed sweep
has bounded buffers, but the future full construction cost remains conditional.

## Acceptance and reproducibility

G0: duplicates-600k, random-4-20k, satellite-18k passed byte identity in
slim resident-dictionary, slim streamed-dictionary, and standalone LCE
modes, all using real samples and `--resolve-ri4`. The retired
`--resolve-parse` path was not used. G1/G3 evidence and exact commands are in
`gates.log`; the unit suite passed 216,000 differential and counter checks.
The C++ binaries use `tools/build_slim_dump.sh` pinned dependencies; Rust
uses the release build. `tools/slim_watch.py` records owned process groups,
RSS, and exit status; `tools/slim_pass3.py` enforces gate order and
`tools/slim_projection.py` reproduces the arithmetic. No git commits.

Rebuild the runner's executable paths with:

```sh
bash tools/build_slim_dump.sh /tmp/laneQ/pass3/slim_dump_cached64
bash tools/build_slim_dump.sh /tmp/laneQ/pass3/slim_lce_probe tools/slim_lce_probe.cpp
(cd xsa && cargo build --release)
```

With the validated samples and completed sample-watcher status present,
`python3 tools/slim_pass3.py` reproduces the ordered gates. Exact preparation
and gate commands are retained in the logs.
