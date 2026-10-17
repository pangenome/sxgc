# sol8a: exact radix slice; work and pile capacity targets remain open

Base `cbcdd28`, branch `sol-8a-radix-sweep`. The bounded radix producer lands
byte-identical containers at 100MB and 1GB through the unchanged publisher and
fused finish. **The 1GB residue sorter performs 157,116,619,752 compared symbols,
2.5976x the retired merge's 60,485,597,061 (+159.76%)**, even after skipping
certified radix prefixes. This is a measured work regression, not a speedup.
At N=8, 92.197% of suffixes remain in repeats. The premise of a small residue
is false on this fixture for the tested widths.

The core text + position window + product subtotal is 2.179TB at pile scale,
but the unchanged finish retains rlebwt for replay: source + rlebwt + container
alone project to 3.851TB. The conservative known-category reserve is 5.634TB
with one position bank and inherited witness/piece allowances. The requested
2–3TB all-phase target is **not established**; the slice is not pile promotion.

## Measurements first and choice of N

`measure.cpp` was implemented and the 1GB measurements committed BEFORE pipeline
code. It uses the same CorpusText view as Keys::cmp, with the exact production
map `/mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin`. 0x1e remains an ordinary
separator byte in the global corpus view: keys are not truncated at document
boundaries. Finite suffix ends are distinguished from legal NUL extensions.
No suffix positions exist in the measurement arrays and no keys are persisted.

| N | singleton fraction | residue suffixes | residue buckets | projected sum b log2 b |
|---|---:|---:|---:|---:|
|3|0.000173813|999826852|637780|17713029102|
|4|0.002146196|997854468|3639145|15095139735|
|5|0.008574975|991425684|10587614|12941960716|
|6|0.023262031|976738619|20695404|11099055219|
|8|0.078026309|921974304|44537360|8059600517|

Choose **N=8**, the smallest projected comparison work among the measured
widths, with the same eight-byte numeric key storage. The largest N8 bucket is
13,568,990 eight-space prefixes. The complete ten largest keys/counts for EVERY
N, exact summaries and logarithmic multiplicity histograms are banked in
[histogram-1b.jsonl](histogram-1b.jsonl). Entry j counts buckets with multiplicity
in [2^j, 2^(j+1)-1]; bin zero is exact singleton count.

The b log2 b projection is a heuristic, not a measured comparison count or
rigorous lower bound. At N8 it predicts ~8.06 billion comparisons; rechecking
eight prefix symbols per comparison would alone project 64.477 billion symbols.
The implementation skips that certified prefix, but long repeats and generic
sorting still make the actual work much larger. The optional 10GB measurement
was deferred: the measurement method needs ~170GB RAM and tenfold work, on a
busy host. No 10GB result is claimed.

## Model and amended D2

See [RADIX_SWEEP_DESIGN.md](../../bit6/sxi_logs/pile-builder-v2/RADIX_SWEEP_DESIGN.md)
for the complete pile arithmetic, phase overlap and unresolved capacity costs.
D2 AMENDED BY THE OPERATOR: positions may materialize transiently in bounded
rolling segments during radix passes; never a full SA, never a persistent
per-suffix position column. No new durable product artifact is introduced.

`pile_model.py` independently counts remapped two-symbol prefixes in bounded
windows and extrapolates the exact slice range-packing rule to 1.31TB. A
32-billion-position disk window occupies 256GB. It projects **45 collecting
segments + one counting sweep = 46 sweeps**, reading 60.26TB, 30,130s=8.369h at
2GB/s. A future position-only disk implementation writes and rereads 10.48TB
each, adding 10,480s; these three transfers total 11.281h before residue CPU,
finish, fingerprint lookup traffic and additional sorting passes. This is a
service-work model, not a wall forecast or implemented disk segmentation.

The largest projected first-two-symbol bucket is 31.245 billion positions,
close to the 32-billion cap; changed corpus distribution may force fallback.
All model inputs and qualification are in `pile-model.json`.

## Landed slice, source locations and position bound

* `bit6/cross_lcp_merge.cpp:1561`: Keys::cmp accepts an explicitly certified
  equal prefix. Default zero preserves old callers and their logical work.
* `:5279`: RadixWork and RAM records; `:5298`: sequential window sweep and
  exact finite two-symbol keys; `:5309`: contiguous key-range planning,
  stable radix partitioning, singleton placement and residue sorting.
* `:5324`: oversized-prefix bounded selection fallback; every selected batch
  is emitted and discarded before the next scan, including periodic cyclic
  keys whose full rotations tie (ascending-position tie break).
* `:5381`: direct producer CLI, existing sparse hash and windowed LCE cache,
  then the UNCHANGED SequentialV2 sink (`:3601`, row interface `:3699`).
* `:5427`: `--radix CORPUS --remap MAP --out-prefix PREFIX` entry point.
  `--radix-bytes 8 --position-cap 32000000 --threads 16` are qualifying flags.

The producer reads no chunks or compressed per-suffix index. Numeric radix
partitioning places singleton keys without suffix comparisons; residues use
the inherited borrowed-window/hash/fully-verified LCE comparator. Ordered
emission is concatenation of key ranges. The existing band/piece/spool formats,
flow window, seam capture, fused finish and chi-container are unchanged.

Two RAM record banks have at most 2B position slots, each with an eight-byte
key and eight-byte position: at B=32 million, at most **1,024,000,000 bytes**
of main record storage. Both banks are destroyed before the next segment.
B is additionally clamped below n/2 (degenerate n<=2 needs one slot). The 1GB
run's observed high water is 63,992,090 slots / **1,023,873,440 bytes**, across
35 collecting segments. No full SA or persistent per-suffix position column
is produced. Scalar/stack temporaries and existing sink/cache memory are
additional to this explicitly bounded record storage.

The Rust front door still selects the inherited route. This is an opt-in
native/runtime vertical slice; cluster scheduling and disk-backed positions
are deferred. Native/runtime sources are byte-identical; all 57 manifest
entries validate. `provenance.json` binds source and tested executable hashes.

## Gates, verbatim

```text
RADIX_MEASUREMENT_ORACLE_PASS cases=20 short_suffixes=1 NUL_padding_collision=1 production_remap=1 full_byte_alphabet=1 exact_multiplicities=1
```

```text
RADIX_ORACLE_PASS cases=3270 fixtures=109 N=3,4,5,6,8 modes=cyclic,dollar cap=1,7,32 NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic_giant_bucket=1 nested_repeats=1 nonidentity_remap=1 exact_brute_SA=1 bounded_selection_passes=59480
```

```text
RADIX_ORACLE_PASS cases=3270 fixtures=109 N=3,4,5,6,8 modes=cyclic,dollar cap=1,7,32 NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic_giant_bucket=1 nested_repeats=1 nonidentity_remap=1 exact_brute_SA=1 bounded_selection_passes=59480
```

```text
RADIX_TINY_PRODUCT_PASS cases=24 fixtures=12 real_windows=3 NUL=1 byte05=1 wrap_prefix=1 periodic=1 nested=1 cap=1,7 raw_runs=brute_exact chi=brute_exact witnesses=all_exact container=validated
```

The two 3,270-case order runs exercise RAM-cache and CROSS_NO_POOL file-window
paths, respectively. The 24 product cases independently sort brute rotations,
compute direct cyclic LCPs and a scalar sweep, compare raw padded RLE bytes,
then compare chi and ALL emitted witnesses. Cases include NUL/05, exact
[0x1e,0x0a,0x1e], periodic text, nested repeats and three real fixture windows.
Exact tiny chi values are in `tiny-product-results.json`.

```text
100m:
SXI2_PASS bytes=48768685 lite=1 members=6
PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515 container=/mnt/nvme2n1/erikg/sol8a-gate-100m-final/product.sxi2
producer_rc=0 finish_rc=0 cmp_rc=0
```

```text
1b:
SXI2_PASS bytes=467814822 lite=1 members=6
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=/mnt/nvme2n1/erikg/sol8a-gate-1b/product.sxi2
producer_rc=0 finish_rc=0 cmp_rc=0
```

Exact reference banks:
`/mnt/nvme2n1/erikg/extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`
and `/mnt/nvme2n1/erikg/extcols-scratch/pilev2-g2-1b/frag.sxi2`.
Final gate commands/return codes/walls are in `gate100m.json` and `gate1b.json`.

Standing outputs (full commands/summaries in `validation.json`):

```text
SPARSE_ORACLE_PASS modes=cyclic,dollar runs=exact seam_spool=exact production_bands=exact trivial_bases=exact terminal=exact legacy_shard_admission=1 WM=absent
SELFTEST_PASS count=200 seed=1 kway=1
SPARSE_ORACLE_PASS modes=cyclic,dollar runs=exact seam_spool=exact production_bands=exact trivial_bases=exact terminal=exact legacy_shard_admission=1 WM=absent
SELFTEST_PASS count=200 seed=1 kway=2
SPARSE_ORACLE_PASS modes=cyclic,dollar runs=exact seam_spool=exact production_bands=exact trivial_bases=exact terminal=exact legacy_shard_admission=1 WM=absent
SELFTEST_PASS count=200 seed=1 kway=8
SWEEP_STATE_CARRY_ORACLE_PASS streams=2048 splits=34785 NUL=1 byte05=1 singleton=1 periodic_colors=1 empty=1 ordered_witnesses=1 final_state=1
CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=3392 workers=4 both_coalescers=1 class_flanks=1 padding_straddle=1 dollar=1 legal_00_05=1
BAND_PROGRESS_READER_ORACLE_PASS trivial_bases=1 band_only=1 deferred_class=1 EOF_clamp=1 terminal=1 delayed_full_band_EOF=1 backward_terminal_refused=1 malformed_refusals=12
SEAM_SPOOL_READER_ORACLE_PASS empty=1 adversarial_orders=2 padded_flanks=1 NUL=1 byte05=1 malformed_refusals=3
FLOW_ORACLE_PASS initial_minus_one=1 oversized_positive_window=1 unsigned_credit=1 blocked_until_consumed=1
{"P": [1, 4, 8, 16], "P8_EOF_permutations": 40320, "P8_completion_orders": 68, "W": 1100, "blocks": 50062, "boundary": "inclusive", "deadlocks": 0, "duplicate_rows": 0, "exact_rows": true, "exact_runs": true, "lost_rows": 0, "peak_bands": 1100, "persistent_recovery": false, "prefix_credit_releases": 35280, "publication": "committed_prefix_only", "publisher_stalls": true, "repeated_cuts": true, "result": "PASS", "rows": 66735, "schedules": 400, "singleton_targets": true, "spool_budget": "global_with_active_batch_reserve", "terminal": "all_ranges_only"}
```

The segment model passes 400 schedules and all 40,320 P8 EOF permutations,
with zero deadlocks/lost/duplicate rows and peak backlog 1100; complete output
is in `segment-model-result.json`. Cargo check passes in 4m31s (13 Rust warnings).

## Work counters, measured

```text
100m: RADIX_WORK N=8 sweep_bytes=500002515 sweeps=5 prefix_ops=500002515 key_ops=100000503 key_bytes=800003996 singleton=14090494 residue=85910009 comparisons=639864223 symbols=9551567601 probes=442927806 segments=4 selection_passes=0 position_cap=32000000 peak_position_slots=63592958 peak_position_record_bytes=1017487328 full_SA=0 persistent_positions=0
```

```text
1b: RADIX_WORK N=8 sweep_bytes=36000023940 sweeps=36 prefix_ops=36000023940 key_ops=1000000665 key_bytes=8000005292 singleton=78026361 residue=921974304 comparisons=9556970969 symbols=157116619752 probes=6856701970 segments=35 selection_passes=0 position_cap=32000000 peak_position_slots=63992090 peak_position_record_bytes=1023873440 full_SA=0 persistent_positions=0
```

The 1GB counters are 9,556,970,969 distinct-position comparisons,
157,116,619,752 compared symbols and 6,856,701,970 inherited probe charges.
Against the supplied retired whole-merge symbol baseline of 60,485,597,061,
this is **2.597587x / +159.7587%**, not a reduction. That baseline was not
remeasured here. Skipping eight certified symbols per comparison avoids
76,455,767,752 redundant prefix checks, already excluded from the new symbol
count; the key work remains separately visible rather than hidden.

`key_ops` counts complete N-byte key constructions; `key_bytes` counts their
actual corpus bytes, excluding finite end padding. `prefix_ops` counts one
two-symbol filtering operation per visited suffix in every sweep, including
the counting pass. The 1GB run performs 36,000,023,940 prefix operations / logical
sweep bytes and 8,000,005,292 key bytes. `counter-summary.json` additionally
journals exact source-derived radix digit operations (two digit accesses per
record/pass, nine passes), histogram-offset additions and bucket-key checks.
These are distinct units, not normalized into a misleading combined total.

Hash construction, corpus cache preloading, seam discovery and finish work
remain outside the comparator-symbol metric, as in the inherited baseline;
this is not a claim about total CPU instructions or physical device bytes.
The source and measured high water enforce the position bound, independently
of sampled filesystem sizes. `attest.py` checks counter conservation,
histogram identity, exact chi/cmp return codes and the record bound.

Observed shared-box walls: 100MB producer 72.396s / completion 82.254s;
1GB producer 1181.824s / completion 1263.951s. Compiler/oracle jobs overlapped
part of 1GB and unrelated host jobs were active. These are not quiet-box
performance measurements or causal timing comparisons with the old merge.

## Residual risks and disposition

* N8 leaves 92.197% residue and the generic in-hand sort has a measured work
  regression. Further refinement/another residue algorithm needs measurement
  before any performance promotion. No claim rejects all radix architectures.
* The 2–3TB all-phase target fails under the unchanged finish's retained
  rlebwt/product overlap at the measured ratios. The 2.179TB core subtotal is
  not advertised as an all-phase reserve. Pile ratios and witness compression
  are extrapolations, not hard worst-case capacity guarantees.
* Disk-backed rolling positions, cluster workers and Rust-front-door routing
  are not implemented. The oversized-prefix fallback is bounded and exact on
  the oracle suite but can be extremely slow on large periodic inputs; existing
  sink seam-work limits can also refuse larger adversarial products.
* No 10GB/pile run or quiet-box performance qualification was performed.
  No persistent worker recovery is added; inherited flow control needs an
  eventually advancing consumer.
* `contact_supervisor` was searched first by name/description in the callable
  catalog and is unavailable. No substitute messaging or scope approval is
  claimed. The in-flight 100GB run's scratch was never opened or modified.

## Branch, reproduction and milestones

Branch `sol-8a-radix-sweep`, base `cbcdd28`. Milestones: `de95e07` (measure first,
operator-amended D2, model), `76be832` (slice and bounded periodic oracle),
`35e5c33` (tiny products and final-source 100MB), `fb615d8` (standing suite and
provenance), `7fdf95c` (Cargo and measurement oracle); final attestation is the
following commit. Exact commands are in the gate/validation JSON files.

```sh
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o /mnt/nvme2n1/erikg/sol8a-measure/radix_sweep
python3 tools/sol8a/gate.py 100m final
python3 tools/sol8a/gate.py 1b
python3 tools/sol8a/tiny_product.py
python3 tools/sol8a/standing.py
python3 tools/sol8a/attest.py
```

Gate roots must be fresh; the producer refuses existing output rather than
clobbering it. Native order oracle build: O2, C++17, pthread, -latomic, source
`tools/sol8a/oracle.cpp`; run with a fresh scratch root, and repeat with
CROSS_NO_POOL=1. Raw logs/binaries remain under `/mnt/nvme2n1/erikg/sol8a-*`.
The initial order harness refused reuse of its generated corpus file; fixture
recreation was fixed before the qualifying runs. No production gate failed.
