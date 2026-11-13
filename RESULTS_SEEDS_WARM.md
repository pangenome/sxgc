# `xsa seeds` warm/batch path and the chain-ready anchor table: RESULTS

Answers [NOTE_FROM_EMENDER_XSA_SEEDS_COST.md](NOTE_FROM_EMENDER_XSA_SEEDS_COST.md):
`xsa seeds` was a **cold, locate-bound** command with no resident variant. It now
has the same shape as the other three served commands (`mem --serve`,
`branches --serve`, `consult`), plus the anchor table the operator chain
(`seeds → colinear chain → correspondence → fork`) consumes.

Branch `feat/seeds-warm`, based on `origin/main` @ `06666b7`.

---

## 1. What was wrong, measured

One 200-byte query, `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2` (SXI2 v6,
63.16 GB), `xsa seeds <c> q200.txt --anchors`, single process, one thread:

| quantity | value |
|---|---|
| wall | 27.5 s (page cache warm from a prior run; 40.6 s on the first, colder run) |
| `rchar` | 1.27 GB (5.9 GB for the four-window set below) |
| `read(2)` calls (`syscr`) | ~1.19 M for one window |
| stdout | 4,950 bytes |
| `SEEDS_STATS` | `scan_ms=170.6 locate_ms=27365.4 positions=280 lf_steps=1226870` |

So **99.4 % of the wall was the locate layer**: 280 reported positions, each one
LF walk to an m11 anchor, averaging **4,382 steps per position** on this
container. A step is a random BWT block decode (`pread` + huffman/gamma decode of
1,024 runs, ~20 µs), which is exactly the 3.3 M `read(2)` / 3 GB that Emender
measured. Nothing about the *answer* (multiplicities are interval widths) needs
those walks; only the reported **positions** do.

---

## 2. The layer table (100 GB heads, 4 windows × 200 B = 1232 anchor rows)

`positions_per_s` is anchor rows per second. "cold" = one classic invocation per
window (4 processes, one thread each, 64 MB arena); "warm" = one `--serve`
process, index opened once, 2 GB decoded-block arena, 8 GB RSS cap.

| layer | what it answers | locates | p50 per unit | cold | warm (-j 64) |
|---|---|---|---|---|---|
| 0 scan | seed length + **exact multiplicity** (interval width) | **0** | 1.24 ms / 200-byte window | same | same |
| 1 anchor locate | `corpus_offset` of one occurrence | 1 LF walk (4,618 steps) | 152 ms / row (cold disk) | 152 ms | **9.9 ms** |
| 2 document resolve | `corpus_doc` from member m15 | 0 | 0.47 ms / row | — | 0.47 ms |

Layers 1 and 2 are measured as `locate_ms / positions`; layer 2 is the
v10-minus-v6 difference on the same windows (12,788 ms vs 12,210 ms of locate at
`-j 64`), i.e. an m15 `rank` walk over delta-varint gaps — no corpus reads and no
extra LF work. Layer 0 is `scan_ms` over the 800 window positions of the batch,
which is why it does not move with the serve mode: it was never the problem.

### Query classes and which layer serves them

| query class | layers | cost |
|---|---|---|
| "how many corpus occurrences does this substring have?" (`--target-mult`, band) | 0 only | 1.24 ms / window, exact, zero locates |
| "where does each seed occur?" (the anchor table, the chain input) | 0 + 1 | 9.9 ms / row at `-j 64`, 67 ms / row at `-j 1` |
| "which document is each anchor in?" (v10 container, m15 present) | 0 + 1 + 2 | +0.47 ms / row |
| same on a container without m15 | 0 + 1 | doc field `-` (absent, never guessed) |

---

## 3. Cold vs warm rate (the gate)

Four windows of 200 bytes, `-j 64 --rss-gb 8 --cache-gb 2`, `--serve` frames in,
one process. Same query set both sides.

### `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2` — SXI2 **v6**, 63.16 GB, **no m15**

```
cold (4 classic invocations, one thread each)
  positions=280  seconds=27.536  positions_per_s=10.2  rchar=1269744955  lf_steps=1226870
  positions=256  seconds=54.141  positions_per_s=4.7   rchar=1740798420  lf_steps=1634174
  positions=400  seconds=55.813  positions_per_s=7.2   rchar=1530237130  lf_steps=1473129
  positions=296  seconds=50.135  positions_per_s=5.9   rchar=1394107606  lf_steps=1355916
  COLD_TOTALS positions=1232 seconds=187.6 positions_per_s=6.6 rchar=5934888111

warm (one --serve process)
  -j 1   seconds=83.480 positions_per_s=14.8  locate_ms=82511.0
  -j 8   seconds=20.787 positions_per_s=59.3
  -j 32  seconds=14.796 positions_per_s=83.3
  -j 64  seconds=13.265 positions_per_s=92.9  locate_ms=12209.6  lf_steps=5690089
         hits=8856791 misses=4838995 rchar=4994231458 syscr=5018650
         peak_rss_gb=7.945 cap_gb=8.000 arena_bytes=2146959360

SEEDS_WARM_RATE cold_positions_per_s=6.6 warm_positions_per_s=92.9 multiple=14.1x
```

### `/mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2` — SXI2 **v10**, 65.53 GB, m15 = 16,119,767 docs

```
cold (4 classic invocations)   positions=1232 seconds=234.0 positions_per_s=5.3 rchar=5934839761
warm -j 1                      seconds=77.390  positions_per_s=15.9
warm -j 64                     seconds=14.280  positions_per_s=86.3
                               lf_steps=5690089 hits=8872407 misses=4823379
                               rchar=4978199979 syscr=4999370 peak_rss_gb=7.902 cap_gb=8.000
warm --windows (declared set)  seconds=13.966  positions_per_s=88.2 doc_absent=0

SEEDS_WARM_RATE cold_positions_per_s=5.3 warm_positions_per_s=86.3 multiple=16.3x
```

**14.1× / 16.3×** the cold per-invocation baseline, at the 8 GB cap. The two
containers report the same `lf_steps` for the same windows (5,690,089), so the
m15 member costs 4 % of the locate wall and nothing else.

Both served forms are the same computation: `--windows` (declared JSONL) and
`--serve` (frames) give the same rows for the same windows, and the four-window
frame output is **byte-identical at `-j 1`, `-j 8`, `-j 32` and `-j 64`**
`sha256 ff10fd61…` (v6) / `18c9678d…` (v10).

---

## 4. Determinism (the tie order)

```
fixture (150 MB, v10)  sha256 windows-TSV=8903dd873de063a5 serve-frames=0cf742c3b6265cb5
                        identical across: two runs, -j 1 vs -j 6, both forms
100 GB v6  serve frames sha256 ff10fd61304f7d49…   (-j 1 = -j 8 = -j 32 = -j 64)
100 GB v10 serve frames sha256 18c9678dfc51692c…   (-j 1 = -j 64)
```

The order is stated in the output's own header
(`tie_order=query_offset,corpus_offset,match_len,multiplicity`) and enforced by
one sort before anything is written, so it cannot depend on the walk order, the
thread count, or the machine.

---

## 5. Reads follow the query set, not the container

| run | windows | positions | `rchar` | bytes / row |
|---|---|---|---|---|
| fixture, 1 window | 1 | 72 | 109,854,932 | 1.53 MB |
| fixture, 4 windows | 4 | 336 | 335,293,553 | 0.99 MB |
| 100 GB v6, 1 window | 1 | 280 | 1.27 GB | 4.5 MB |
| 100 GB v6, 4 windows | 4 | 1232 | 4.99 GB | 4.1 MB |

Four windows read 3.05× one window (the query set grew 4×), and the per-row
figure is the LF walk (~4,600 steps × a block payload), not the container: a
4-window batch on a 63 GB container reads 8 % of it, and the same batch on a
65 GB container reads the same bytes. This is the structural difference from
consult's 450× result, stated plainly: **consult's cascade answers without
locating (99.08 % zero-locate rows), and `seeds` cannot** — its output *is*
positions. The warm path removes the per-invocation open and parallelises the
walks; it cannot remove the walks.

The m11 sampling stride (one anchored position per 1,024 BWT runs) is what makes
a walk 4,618 steps. A denser position sample is a **container** decision
(m11 header), not a command one, and is the one change that would move layer 1
again.

---

## 6. Gates

`tools/binbwt/tests/seeds_warm_gate.py` (oracle: `tests/seeds_anchor_oracle.c`,
one mmap pass per pattern with `memmem`, overlapping occurrence **starts**; the
document map it checks against is computed independently from the corpus' 0x1E
separators, never from m15).

```
$ gcc -O2 -o seeds_anchor_oracle tools/binbwt/tests/seeds_anchor_oracle.c
$ python3 tools/binbwt/tests/seeds_warm_gate.py $XSA /tmp/sw-fx/t150m-v10.sxi2 \
      /dev/shm/t150m.txt --out /tmp/sw-gate/final-v10 --docs present --jobs 6
W1 the three input forms answer the same rows
  rows file=96 windows=336 frames=336
W2 every row is an EXACT anchor (oracle)
  patterns=336 rows checked
  rows all=336 capped=42 (multiplicity invariant)
W3 corpus_doc comes from m15, and is ABSENT when the container has none
  m15 present: 336 doc fields verified against the 0x1E scan
W4 determinism: same input twice, and -j 1 vs -j N
  sha256 windows=8903dd873de063a5 serve=0cf742c3b6265cb5
W5 reads follow the query set, not the container
  rchar 1 window=109854932 4 windows=335293553 ratio=3.05 positions=336 bytes_per_row=997897 container=128266224 frac_container=2.6140
W6 the declared-window round trip, the anchors-all mode, and the refusals
  declared windows replayed, every-occurrence mode larger, XSWIN001 == JSONL, 6 refusals by name
SEEDS_WARM_GATE_OK
```

The same gate on the **no-m15** container (`/tmp/sw-fx/t150m-v6.sxi2`, built from
the 150 MB fixture, `--docs absent`) passes with
`m15 absent: all 336 doc fields marked '-'`.

W2 is the exactness gate: for every one of the 336 rows, the C oracle's
overlapping occurrence count equals the reported `multiplicity` and the reported
`corpus_offset` is one of the oracle's occurrence starts. With
`--anchor-max-per-seed 1` the row set shrinks to 42 and **no multiplicity
changes** — an output cap is not a support threshold.

### Regression: the classic path is byte-identical

`xsa seeds <container> <text>` (no new flags) against the pre-change binary
(`sha256 4d078ae5…`, built from `06666b7`):

| input | pre-change sha256 | new sha256 |
|---|---|---|
| fixture, default TSV | `283bffb36b8042ad…` | `283bffb36b8042ad…` |
| fixture, `--jsonl` | `2820635d9eef21c6…` | `2820635d9eef21c6…` |
| fixture, `--stride 3 --jsonl` | `12826beaab27c75b…` | `12826beaab27c75b…` |
| fixture, band mode | `2564d6a2a243f30b…` | `2564d6a2a243f30b…` |
| fixture, `--nearest` | `7f8d4c603ba680ef…` | `7f8d4c603ba680ef…` |
| fixture, `--skip-markup --jsonl` | `2820635d9eef21c6…` | `2820635d9eef21c6…` |
| fixture, `--limit 32` | `4f8b018954acd253…` | `4f8b018954acd253…` |
| fixture, `--max-seeds 8 --max-len 40 --min-len 10 --jsonl` | `3645d353e6daee48…` | `3645d353e6daee48…` |
| **100 GB v6**, 200 B text, TSV | `9bc903a29acca9c6…` | `9bc903a29acca9c6…` |
| **100 GB v6**, 200 B text, `--jsonl` | `368b944a65ba6536…` | `368b944a65ba6536…` |
| **100 GB v10**, 200 B text, TSV | `9bc903a29acca9c6…` | `9bc903a29acca9c6…` |
| **100 GB v10**, 200 B text, `--jsonl` | `368b944a65ba6536…` | `368b944a65ba6536…` |

`SEEDS_STATS` is also identical modulo `scan_ms`/`locate_ms` on every one of
those runs (all counters, the multiplicity histogram and every window outcome).

---

## 7. The anchor table, and the two containers

```
$ xsa seeds $C --windows wins.jsonl --corpus pile-head100.txt -j 64 --rss-gb 8 --cache-gb 2
#xsa-seeds-anchors v1 mode=target target_mult=50 cap=none min_len=12 max_len=64 stride=1 \
  max_seeds=64 anchor_max_per_seed=8 docs=m15:16119767 \
  tie_order=query_offset,corpus_offset,match_len,multiplicity
window_id	query_offset	corpus_doc	corpus_offset	match_len	multiplicity
0	0	5665591	35185667307	26	69
...
```

* `corpus_doc` is member m15's document at the located offset on the v10 head; on
  the v6 head (no m15) every row carries `-` and the header says `docs=absent` —
  a docid is never inferred from a sidecar, a --docs directory or the content.
* `multiplicity` is the interval width, always. `-k/--limit/--anchor-max-per-seed`
  are named as OUTPUT CAPS in `--help`.
* Serve mode sends the same table as fixed-width frames (u64 window id, u32 rows,
  u32 seeds, then 32-byte rows), one u32-length-prefixed frame per window — the
  `mem --serve` encoding, verified once at open.

---

## 8. Measurement notes (what else ran on the box)

All walls were taken on a 256-core box with `load average` between 10 and 19
throughout: the pile build's `scatter_sort` and `base_scatter_sort` at ~100 % CPU
each and `tools/names_extract.py` at ~100 % (idle priority, untouched). Individual
`xsa seeds` runs used **≤ 10 cores** (measured `%P` 99 % at `-j 1` to 957 % at
`-j 64`), so the rates above are not limited by the box's other work. The
container page cache held the heads after the first pass (`/proc/diskstats`
showed 111 MiB of physical reads during a 5 GB `rchar` run), so the "cold"
column is a cold **process**, not a cold disk, except where noted.

Scratch used by the gates: `/tmp/sw-fx` (fixture containers), `/dev/shm/t150m.txt`
(the repo's standard 150 MB fixture), `/tmp/sw-gate` (gate outputs). No text was
written anywhere: the corpus is read through a windowed mmap / one bounded
`pread` buffer, and the only things that touch the filesystem are the small TSV
answer files, the declared window manifests and the anchor tables themselves.

## 9. Residual risks

1. **Layer 1 is still one LF walk per reported row**, so the cost is
   index-proportional in *anchor rows* (≈1 MB of container read and ≈10 ms at
   `-j 64` per row on the 100 GB heads). `--anchor-max-per-seed 0` (every
   occurrence) multiplies that by the multiplicity; the default 8 rows per seed
   keeps a 200-byte window near 0.5 s at `-j 64`.
2. **Thread scaling saturates around 10 cores** on this box for this workload
   (`-j 8` → 4.6 cores, `-j 64` → 9.6 cores, 6.3× wall). The pool is explicitly
   sized and the work unit is one row, so the remaining limit is inside the
   block-cache/decode path (one decode per LF step), not in the dispatch. Not
   resolved here; the arena size (`--cache-gb`) is the one knob that moved it,
   and *smaller* was better (2 GB fast, 25 GB 4× slower).
3. **`--rss-gb` at 8 GB costs ~20 %** versus an uncapped run
   (`drops=396` governor bleeds): 15.9 s vs 12.6 s on v6. The cap is honoured,
   the price is the page-table drop.
4. A window's own scan is serial (1.24 ms per 200-byte window), so a very large
   single window is not parallelised internally; batches of ordinary windows are.
