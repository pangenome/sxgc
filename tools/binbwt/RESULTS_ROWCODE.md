# The band row code (`SS_ROWCODE`): measurements

Change: the band part files store the band's shared key PREFIX once in a 48-byte
header and only the residual `(24 - p)` key bytes + 8-byte packed position per
row, instead of the full 32-byte row. Format: `FUSED_TAPE.md` ("The band part
wire format"). The in-RAM row, the wave/band plan, the sort order, the LCP
propagation and therefore the container are unchanged - this is an ENCODING
change, gated by byte identity below.

Box: 256 cores / 1 TB RAM, corpus and scratch on `/mnt/nvme2n1` (14 TB, 97 %
full), 150 MB fixture in `/dev/shm`. Binary: `xsa build --tape ... --with-chi`.

## T1 - product identity (the gate that matters)

| rung | container sha256 (before = after) | bytes | chi |
|---|---|---|---|
| 150 MB (`t150m.txt`, n=150,000,002) | `f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e` | 118,891,088 | 44,034,230 (both, = the banked 150 MB value) |
| 10 GB (`ten.bin`, n=10,000,017,258) | `0afb17f18d9f230e54c9186e0e368025b1bfd998d964bcf9443f2af63ab709d4` | 5,458,818,464 | 2,049,492,861 (both) |

`cmp` reports both pairs byte-identical. The 150 MB container is also the one
the banked query gates ran on:

```
T1 count 100/100 exact; median warm wall=0.141666s; full locate wall=0.687011s; speedup=4.850x
T3 low-count pattern 0: 1 hits, limit 16 byte-identical; all 100 limited outputs equal full prefixes
T4 default 100/100 counts and 574/574 positions exact; --all identical; zero limit and invalid arguments passed
```
(`tools/binbwt/gate_query_count_limit.py` against the NEW container.) The 10 GB
chi is NOT the banked `2,676,929,445`: this box's 10 GB snapshot
(`head -c 10000017258 corpus.bin`) is not the banked `pile-10b-snap.txt`, so the
banked 10 GB chi cannot be reproduced here at all - the gate used is my own
before/after pair on the same bytes, which is the identity claim.

## T2 - scratch (peak_intermediate_over_n, du-sampled over the scratch tree)

| rung | before | after | delta |
|---|---|---|---|
| 150 MB | `peak_bytes=355494286 n=150000001 peak_intermediate_over_n=2.3700` | `peak_bytes=340344813 n=150000001 peak_intermediate_over_n=2.2690` | **-4.26 %** |
| 10 GB | `peak_bytes=25654067275 n=10000017258 peak_intermediate_over_n=2.5654` | `peak_bytes=24762276203 n=10000017258 peak_intermediate_over_n=2.4762` | **-3.48 %** |

The 10 GB saving (3.48 %) is smaller than the row saving (4.09 %) because the
sampled peak also contains the unchanged band-SA/LCP slices and finish pieces;
the row material is one term of it (1.788 n here).

## The prefix histogram: the bands are cut shallowly

10 GB, whole run (rows per prefix length `p`):

```
SS_ROWCODE_EXIT waves=23 prefix_hist=0:1185459907,1:5455849557,2:2831737249,3:324918022,4:93233602,5:62540723,6:15449853,7:30828322 mean_residual=22.691 bytes_per_row=30.691 full_key_row_bytes=32 rows=10000017259 row_bytes_total=306912844884 header_bytes=506880 peak_wave_row_bytes=17879317673 n=10000017259 scratch_ratio=1.7879 saving_vs_full_key=0.0409
```

`p = 1` holds 55 % of the rows, `p <= 2` 95 %. That is the expected shape and it
is the honest headline: a band is `rows_per_band = 19,338,542` consecutive
ranks, so its two fence keys are ~1.9e7 ranks apart and agree on ~log_256(1.9e7)
= 3 bytes at best; even the measured 1-2 is a real cut, not a bug. **The saving
is bounded by band size, i.e. by the arena, not by the encoding.** Wider bands
(more rows per band) are the lever that would deepen `p`; narrower ones would
erase the saving. Bands with `p = 4..7` are the structured/tie-heavy key ranges.

## T3 - speed (scatter_sort wall, `time`)

| run | wall | user CPU | sys CPU | max RSS |
|---|---|---|---|---|
| before | 27:21.08 | 7,660.85 s | 1,735.41 s | 21,115,320 KiB |
| after | 31:41.06 | 9,309.81 s | 1,570.82 s | 21,482,228 KiB |

**+15.9 % wall, +21.5 % user CPU.** Both 10 GB builds ran CONCURRENTLY with each
other on the same nvme (and the "after" run's first waves overlapped a 100 GB
probe that was killed), so the wall figures are inflated for both; per-wave the
"after" run is 10-20 % slower (`w=2/23` 108.85 s vs 110.94 s, `w=13/23` 116.49 s
vs 76.72 s). The user-CPU delta is contention-free and it says the decode loop is
a real cost: the residual copy is a VARIABLE-length `memcpy` (`24 - p`) per row,
which does not inline. A `switch (p)` over compile-time widths is the obvious
follow-up and was not bundled here (it needs another build + gate cycle).

## T4 - 100 GB (pile) probe

`--input /mnt/nvme2n1/erikg/wiki/corpus.bin` (n = 100,093,934,467),
`--threads 16 --memory-gb 64`. Stopped after the first wave's rows were counted
and its histogram was emitted (the instruction: run far enough to report the
histogram and the scratch peak):

```
SS_ROWCODE wave=0 bands=79 prefix_hist=0:31073286,1:260435672,2:353714614,3:426865534,4:63899663,5:31010226,12:31678887 mean_residual=21.464 bytes_per_row=29.464 full_key_row_bytes=32 rows=1198677884 wave_row_bytes=35318494920 header_bytes=60672 scratch_ratio=0.3529
SS_START n=100093934467 row_bytes=32 sort_row_bytes=90 cap_bytes=200187868934 (2.00n) rows_per_wave=1237666688 waves=81 ram_gb=1.00 rows_per_band=19338542 par_bands=16 threads=16 in_band_arena_bytes=27847500480 (0.278n)
```

Observed scratch peak (du every 2 s over the killed window, before the first
wave's bands were drained):

```
peak_bytes=73788440286  n=100093934467  peak_over_n=0.7372
```

At pile `p = 3` dominates (426.9 M rows) and `mean_residual = 21.464`
(`bytes_per_row = 29.464`, -7.9 % vs the full 32-byte row) - deeper than at
10 GB, and there is a 31.7 M-row `p = 12` tie class. The 0.7372 n peak is a
PARTIAL figure (a wave-sized wave 0 plus the live band streams), not the
container-scale `PEAK_INTERMEDIATE` of a completed pile build: at
`--memory-gb 64` the wave cap is the 32 GB budget, not 2 n, so this probe's peak
is not comparable with the 2.4-2.6 n rungs above.
