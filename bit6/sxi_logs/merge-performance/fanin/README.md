# FAN-IN LANE (merge perf): fixed small chunks + repeated k-way fan-in

This subdirectory holds the fan-in work: the shape the user asked for —
fixed small chunks + repeated k-way fan-in, delete-as-you-go, bounded peak —
on top of the certified flat k-way core. Byte-identity + chi is the only
arbiter; every run carries caps (timeout + `ulimit -v`).

## Driver surface (already in `xsa build`, commit c6ddc1d)

* `--chunk-mb N` — fixed chunk size (MiB): chunk count = `ceil(bytes / N MiB)`.
  Mutually exclusive with `--chunks`.
* `--fanin` — repeated k-way fan-in: fixed chunks + levels
  `count -> ceil(count/--kway) -> ... -> 1`, every level the SAME certified
  k-way merge. Requires `--chunk-mb`. Auto-sets `CROSS_DELETE_CONSUMED=1`.
* `CROSS_DELETE_CONSUMED=1` — in `cross_lcp_merge`, once a group merges its
  inputs (`.crle` + `.pos` + `.ref` + legacy `.sxs`) are unlinked. The fan-in
  driver turns it on; the flat/rung shapes leave it off so the gates can
  compare intermediates.

`XSA_FANIN_LEVEL` lines are the PLAN projection; the runtime per-level walls
are the `CROSS_KWAY`/`CROSS_PAIR` lines in `merge.log` (paths carry `L0-`,
`L1-`, ...). Peak disc is the `MERGE_PEAK_DISC_USED_BYTES` sample (scratch
tree polled every 2 s during the merge), which catches the mid-merge peak that
a phase-boundary `df` cannot.

## Item 1 — 100MB SHAPE MATRIX (`matrix-100m.sh`)

Fixture: `mergperf-100m-snap.txt` (n=100 000 503), 16 chunks (6 MiB), banked
chi = 29716349 (N=100000503, R=38647515). Reference: `mergperf-100m-bank`
(the certified v3 flat artifact). Shapes × modes, all one binary (a7ec3df9):

| shape        | levels | mode    | 8/8 bytes | chi | merge wall | peak merge disc | end tree |
|--------------|--------|---------|-----------|-----|------------|-----------------|----------|
| flat k=16    | 1      | dense   | PASS      | OK  | 52.7 s     | 3.05 GB         | 3.90 GB  |
| fan-in k=4   | 2      | dense   | PASS      | OK  | 66.9 s     | 2.95 GB         | 3.02 GB  |
| fan-in k=2   | 4      | dense   | PASS      | OK  | 98.4 s     | 2.91 GB         | 3.02 GB  |
| flat k=16    | 1      | compact | PASS      | OK  | 48.3 s     | 2.44 GB         | 3.43 GB  |
| fan-in k=4   | 2      | compact | PASS      | OK  | 69.9 s     | 2.48 GB         | 3.02 GB  |
| fan-in k=2   | 4      | compact | PASS      | OK  | 106.5 s    | 2.61 GB         | 3.02 GB  |

(Full numbers + per-level telemetry: `matrix-100m.results`.) At 100MB the
chunk set is small, so the delete-as-you-go peak saving is modest
(2.95 vs 3.05 GB dense); the end-of-build tree is smaller for fan-in
(3.02 vs 3.90 GB) because the consumed chunks and per-level intermediates
are gone. The walk collapses with k at this scale: the k=4 level-0 groups walk
~25 MiB in 3.6-5.4 s (~5-7 MiB/s) while the ONE flat k=16 walk walks 100 MiB
in 37.7 s (~2.7 MiB/s) - deeper interleave is slower per byte even though all
pool hit rates are >99.9% here (the 57% collapse the README records is a 1GB
k=16 effect; see `gate-1b.results`).

## Harvested run (predecessor's orphan)

`fanin-100m-k4` (xsa build --chunk-mb 6 --kway 4 --fanin, current binary
a7ec3df9) completed and was harvested: 8/8 artifacts byte-identical to
`mergperf-100m-bank`, chi = 29716349 exact, peak merge disc 2.95 GB.

## Item 2 — 1GB STRONG GATE (`gate-1b.sh`)

Fixture: `mergperf-1b-snap.txt` (n=1 000 000 665), 10 chunks (100 MiB), banked
chi = 283296933 (N=1000000665, R=368036609). Reference: `mergperf-1b-B`.
FLAT k=10 (one merge) vs FAN-IN k=4 (levels 10 -> 3 -> 1). The fan-in's
level-0 groups are 4 x 100 MiB = ~400 MiB sides — the top-level-side
datapoint for the walk-vs-side-size curve. Numbers in `gate-1b.results`.

RESULT: BOTH GREEN. flat k=10 = 10/10 byte-identical + chi exact (merge
22:45, peak disc 30.5 n). fan-in k=4 = 10/10 byte-identical + chi exact
(levels 10->3->1, merge 43:17, peak disc 30.5 n). Delete-as-you-go verified:
the chunk and mwork dirs are empty after each fan-in run.

## Item 3 — delete-as-you-go peak disc

Measured (`MERGE_PEAK_DISC_USED_BYTES`, 2 s sampler): 100 MB flat 30.5 n vs
fan-in k=4 29.5 n vs fan-in k=2 29.1 n (dense); compact 24.4 / 24.8 / 26.1 n.
1 GB flat k=10 30.5 n and fan-in k=4 30.5 n — identical. The peak is
output-bound, not input-bound; see `CORPUS_SCALE_OUTPUTS_SURVEY.md`.

## STATUS / HARVEST (for the next lane member)

* Item 1 COMPLETE (100 MB shape matrix, all six shapes green).
* Item 2 COMPLETE (1 GB strong gate, both shapes green).
* Item 3 measured; item 4 table in `MEASUREMENT_TABLE.md`; item 5 survey in
  `CORPUS_SCALE_OUTPUTS_SURVEY.md` (the critical-path finding).
* QUEUED, IN FLIGHT: `curve-1b.sh` (same-binary 1 GB walk curve: k=16 @62.5,
  k=20 @50, k=5 @200, k=2 @500). It writes `curve-1b.results`. Harvest it,
  fill the curve rows in `MEASUREMENT_TABLE.md` §1, and state the curve as
  (side size, k, total live bytes) — same-binary provenance per cell.
* OPEN DECISION (blocking the pile): the shipped-container contract. The
  peak is set by corpus-scale outputs; dedup + bit-pack + streamed finish
  floors at ~5-8 n (6.5-10.5 TB), product-only at ~1.5-2.6 n. Supervisor to
  choose what must ship; then the dedup tier (drop `head_sa`, `ssa_t`; trim
  `agg` to topLCP/interiorMin) is the first re-gated step.
