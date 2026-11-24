# Native-wide dictionary component grid (not a product gate)

From this checkout, after the 10 GB full-product and three 1 GB arms have
completed, with no concurrent managed timing/build work:

```sh
bash gates/overlap_ten_dict_grid.sh "$RECEIPT10" "$PAIR1" "$DEADLINE"
```

`DEADLINE` is an absolute Unix timestamp in seconds, not a duration. `PAIR1`
contains `comparison.json` and `batch.json` from `overlap_radix_product.sh
--include-batch`; speed misses are allowed, incomplete or unverified arms are
not. The grid refuses rather than waiting for prerequisites. A nonblocking
lock shared with the 10 GB product gate and peer-process checks supplement the launcher's obligation to
reserve exclusive timing. The deadline caps compilation and fixtures. The worker requests cooperative stop at the next boundary (never a signal inside libsais); admission reserves at least 4,200 seconds.
Requires Linux cgroup v2, user systemd, GCC/libgomp, local Cargo dependency
cache and the pinned source `sandbox/packed-front-10g-YebiCY/columns`. Copy,
private eviction/mincore verification and producer share one 200 GiB, 400%,
no-swap service. Nothing evicts/removes banked columns. No 100 GB mode.

The binary is a private release build with the `dict-grid` Cargo feature and
`XSA_BUILD_DICT_OPENMP=1`. The harness alone sets `XSA_RESIDENT_DICT_GRID=1`,
packed native-wide switches, passive OpenMP waiting, no spinning and 4-thread
limits. `stdout` is `/dev/full`; there is no product or auditor. The 140 GiB
array grant is checked with the 4-thread plan. Retained producer lists remain
live. All arms see the same encoded bytes. Each timed region covers allocation,
zero-initialization and sorting; excludes prep, SHA and deallocation. The
process CPU clock includes OpenMP threads; kernel `VmHWM` resets before each
arm; `/proc/self/io` measures attributed physical bytes, not cache traffic.
`selected_threads` is an API argument, not an observed active team size.

`resident.log` records per-arm counters, full-entry digest and hash seconds;
`inputs.json`, `exit.json`, `binaries.log` and `cold.log` describe resources,
provenance and private initial state. `result.json` selects the fewest threads
within 5% of the fastest arm, requiring neither additional reads nor writes
relative to serial (no candidate: null, exit 75). Touch `stop.requested` in
the receipt to stop. Failed/interrupted receipts are evidence, not scores.

Only one ordering per thread count: fixed 1→4→2 order can retain page/cache,
NUMA placement and runtime pool effects. Equal SHA-256 across all i64 LE entries
is not independent product or native-wide order certification; preceding
verified product/oracle evidence remains required. No end-to-end speedup,
physical IO reduction or scale promotion follows from this grid.
