# Recursive sampled tape planner qualification

Implementation: `a8a7188`, analytic-fence correction `40177b1`, based on `306f0ea`. Evidence root: /mnt/nvme2n1/erikg/recursive-planner.

Both standalone and packaged sorters use positional stride samples with multiplicity, recursively extended prefix fences (24 through 192 bytes), an 80% planning target, and an actual-row assertion at 110% of the target. Each existing byte-range wave scatters once. The elapsed-time split heuristic and its thresholds are removed.

Analytic termination requires a direct corpus proof of a single-symbol suffix ending at EOF, plus an exclusion check for matching internal prefixes and a runtime membership check. Analytic rows reverse scatter position order without sorting. Position descending means **shorter suffixes first** for this finite suffix comparator. Sample byte agreement is never sufficient.

## R1: exact products

Both release runs use the final binary (`bin/scatter_sort.release`, digest in `release-digests.json`). Products were compared with `cmp` against the retained `tape-redesign/reviewed-*` baselines.

### release-8mb

```json
{
  "reason": "complete",
  "returncodes": [
    0,
    0,
    0
  ],
  "finish_done": [
    "XSA_FINISH_DONE chi=6544643 n=8000002 r=7966303 container=/mnt/nvme2n1/erikg/recursive-planner/release-8mb/product.sxi2"
  ],
  "product_sha256": "2beb35a0d37a6ff6453b108c4d0a99e8eff4af91cf2875643ae6af86c9539f57",
  "depth_histogram": {
    "24": 35,
    "48": 0,
    "72": 0,
    "96": 0,
    "120": 0,
    "144": 0,
    "168": 0,
    "192": 0
  },
  "max_estimated_band_rows": 380000,
  "max_completed_band_rows": 381521,
  "row_bound_violations": 0,
  "max_observed_wait_s": 0.02
}
```

### release-150mb

```json
{
  "reason": "complete",
  "returncodes": [
    0,
    0,
    0
  ],
  "finish_done": [
    "XSA_FINISH_DONE chi=44034230 n=150000002 r=57279194 container=/mnt/nvme2n1/erikg/recursive-planner/release-150mb/product.sxi2"
  ],
  "product_sha256": "9ba2b1a11b02397e8b8a3992bb3366e933a35d1929b0849898178ecc7f31a756",
  "depth_histogram": {
    "24": 37,
    "48": 0,
    "72": 0,
    "96": 0,
    "120": 0,
    "144": 0,
    "168": 0,
    "192": 0
  },
  "max_estimated_band_rows": 7124979,
  "max_completed_band_rows": 7144248,
  "row_bound_violations": 0,
  "max_observed_wait_s": 0.04
}
```

The small corpora require depth 24 only. These histograms do not demonstrate deeper descent; the direct regression exercises depths 48 and 72, and the large census provides the corpus-scale recursion evidence.

## Direct checks

- Both compiled sorter copies pass six independent Python finite-suffix/BWT oracle fixtures. The standalone suite also checks analytic emission and a cap/row-bound refusal.
- The C++ planner regression runs against both source copies: positional strides 1 and 3, recursion to depth 72, analytic suffix ordering, internal runs rejected as analytic, finite tails, and explicit depth-cap cases.
- A standalone analytic LCP run verifies all 1801 SA positions and all 1801 LCP values, including escape records and the normal-to-analytic band seam.
- Any-credit admission, ordered K=1 admission, and nonterminating wait-diagnostic regressions pass.
- Both C++ sources compile with `g++ -O3 -std=c++17 -pthread`; `git diff --check` passes.

## Measurement conditions and limits

Large gates use 96 threads, a 48-GiB sorter envelope, K=32, and `XSA_TAPE_WAIT_S=180`. Large runs overlap; their timings are observed end-to-end measurements, not isolated throughput benchmarks. The runner records commands, return codes, products, depth counts, actual row bounds, nearest-rank p99/median sort times, analytic leaves, cap defects, and the five slowest completed bands. Earlier exploratory runs were interrupted and are not the qualifying large results.

Row bounds do not bound full suffix-comparison time. Very long internal repetitions may reach the 192-byte cap; they are deliberately diagnosed rather than silently accepted as terminal. A very large analytic leaf still faces the strict row-bound assertion. Sampling estimates can be wrong; the actual-row assertion is the fail-loud check. Legacy LCP transport proof gaps described in DEADLOCK_FREEDOM.md remain outside this change.

## R2: complete, required chi passed

All three processes exited zero; 1211 bands were published and consumed. The full depth census reaches 192 bytes, with zero cap defects and zero row-bound violations. These are measured seconds, including the 83.966951-second maximum wait; this is not a claim of subsecond latency.

```json
{
  "reason": "complete",
  "seconds": 1636.746805481147,
  "returncodes": [
    0,
    0,
    0
  ],
  "max_observed_wait_s": 83.966951,
  "max_completed_wait_s": 83.966951,
  "top5_sort": [
    {
      "band": 262,
      "rows": 5760468,
      "read_s": 0.467508,
      "sort_s": 115.221183,
      "write_s": 0.042266
    },
    {
      "band": 24,
      "rows": 8441764,
      "read_s": 0.897064,
      "sort_s": 67.94759,
      "write_s": 0.048234
    },
    {
      "band": 378,
      "rows": 8587759,
      "read_s": 0.99636,
      "sort_s": 22.115349,
      "write_s": 0.049395
    },
    {
      "band": 31,
      "rows": 8818011,
      "read_s": 1.037102,
      "sort_s": 20.072604,
      "write_s": 0.049194
    },
    {
      "band": 30,
      "rows": 6348009,
      "read_s": 0.71088,
      "sort_s": 17.013734,
      "write_s": 0.035112
    }
  ],
  "published_bands": 1211,
  "consumed_bands": 1211,
  "splits": [],
  "finish_done": [
    "XSA_FINISH_DONE chi=2676929445 n=10000017259 r=3474889847 container=/mnt/nvme2n1/erikg/recursive-planner/qualified-10gb/product.sxi2"
  ],
  "incomplete_waits": false,
  "depth_histogram": {
    "24": 1190,
    "48": 6,
    "72": 4,
    "96": 2,
    "120": 2,
    "144": 2,
    "168": 2,
    "192": 3
  },
  "order_analytic_leaves": 0,
  "tape_leaf_at_cap": 0,
  "sort_median_s": 3.601568,
  "sort_p99_s": 10.896901,
  "sort_p99_median_ratio": 3.025599127935388,
  "max_estimated_band_rows": 8445000,
  "max_completed_band_rows": 8975987,
  "row_bound_violations": 0,
  "product_sha256": "97dbf45dc6ad42aaf124a566061a818d0a7f1bc72cc91368b883546d71f899bf"
}
```

## R3: failed latency gate; stopped by operator

The final-binary 100 GB run did not reach band 400. It was stopped after the producer reported a 180-second queue wait. Band 45 was still sorting despite passing its row bound. This demonstrates that the requested row-bounded recursion does not itself bound full-comparator latency.

```json
{
  "reason": "operator-interrupt",
  "seconds": 489.76174265891314,
  "returncodes": [
    -15,
    -15,
    -15
  ],
  "max_observed_wait_s": 180.006,
  "max_completed_wait_s": 26.92658,
  "top5_sort": [
    {
      "band": 78,
      "rows": 8240782,
      "read_s": 1.138715,
      "sort_s": 47.974206,
      "write_s": 0.088271
    },
    {
      "band": 28,
      "rows": 8559010,
      "read_s": 1.121279,
      "sort_s": 47.918004,
      "write_s": 0.052821
    },
    {
      "band": 44,
      "rows": 5949921,
      "read_s": 0.688131,
      "sort_s": 39.828847,
      "write_s": 0.110231
    },
    {
      "band": 88,
      "rows": 8846123,
      "read_s": 1.118158,
      "sort_s": 31.607037,
      "write_s": 0.058936
    },
    {
      "band": 87,
      "rows": 8246131,
      "read_s": 1.236123,
      "sort_s": 24.938546,
      "write_s": 0.052394
    }
  ],
  "published_bands": 93,
  "consumed_bands": 62,
  "splits": [],
  "finish_done": [],
  "incomplete_waits": true,
  "depth_histogram": {
    "24": 144,
    "48": 0,
    "72": 0,
    "96": 0,
    "120": 0,
    "144": 0,
    "168": 0,
    "192": 0
  },
  "order_analytic_leaves": 0,
  "tape_leaf_at_cap": 0,
  "sort_median_s": 9.692489,
  "sort_p99_s": 47.974206,
  "sort_p99_median_ratio": 4.949627077214119,
  "max_estimated_band_rows": 8432751,
  "max_completed_band_rows": 9686482,
  "row_bound_violations": 0,
  "stalled_band": {
    "band": 45,
    "state": "sort",
    "elapsed_state_seconds": 297.3082666397095,
    "row_bound": [
      "TAPE_ROW_BOUND wave=0 band=45 rows=8384125 target=10558004 ok=1"
    ],
    "diagnostics": [
      "SS_DIAGNOSTIC queue=band capacity=32 reservations=32 next=94 wall_s=180.006"
    ],
    "last_waits": [
      "SS_WAIT producer=scatter_sort blocks_on=band.queue band=62 reservations=32 wall_s=2.006",
      "SS_WAIT producer=scatter_sort blocks_on=band.queue band=63 reservations=32 wall_s=2.007",
      "SS_WAIT producer=scatter_sort blocks_on=band.queue band=94 reservations=32 wall_s=2.004"
    ]
  },
  "measurement_note": "Maximum observed wait includes periodic diagnostic lines; in-progress waits are lower bounds."
}
```

## Final analytic-fence edge correction

The release binary adds a normal zero-estimate range below a first analytic leaf, so an unsampled lower key cannot enter that leaf. A stride-2 regression (`z`, byte 1, then 1800 `a` bytes) exercises the missed key. Both source copies pass the regression. The release small-product gates were rerun after this correction. The large runs use the preceding binary, whose only sorting/planning difference is this conditional analytic-leaf fence; neither large run emitted an analytic leaf. Their nonanalytic path is unchanged. The report parser was also corrected to include periodic diagnostics in maximum observed wait time.
