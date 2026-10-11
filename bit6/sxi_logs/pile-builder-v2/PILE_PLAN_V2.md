# PILE PLAN v2 — the plan-first pile launch (G5)

The pile: `/mnt/nvme2n1/erikg/pile.txt`, 1,307,910,802,540 bytes (1.218 TiB,
1.247e6 MiB; 1308x the 1GB gate fixture). The plan below is the v2
pipeline's per-term table, measured at the 1GB G2/G3 gates (branch
`pile-builder-v2`) and extrapolated to the pile. Every pile number below is
**EXTRAPOLATED** unless explicitly marked **MEASURED**; the 1GB numbers are
**MEASURED** (gate evidence in `sxi_logs/pile-builder-v2/g2-1b/`,
`g3-1b/`, `m3-100m/`).

The dry run (model = `xsa/src/plan.rs`, constants versioned in-source,
output in `g5-plan/pile-dry-run.log`):

    xsa build --input /mnt/nvme2n1/erikg/pile.txt \
              --scratch /mnt/nvme2n1/erikg/extcols-scratch \
              --chunks 64 --kway 64 --threads 48 --memory-gb 700 \
              --scratch-free-pct 0 --plan-only --v2-pipeline

Verdict it prints (XSA_PLAN v=4, constants from the 1GB packed-WM
gate xsa-pa-wmpack-1b, 2026-10-11): **FEASIBLE (v2 output side):
scratch-tree peak ~2406 GB (1.975n, bound 2.5n) fits 2860 GB free;
whole-box (corpus + tree) ~3624 GB = 2.975n** — 3624 GiB = **~3.9 TB
whole-box** (decimal). Wall projection at 48 threads: **1401.8 h ~ 58.4
days** end-to-end (EXTRAPOLATED from 1GB-measured constants, and
page-cache-optimistic: the finish's corpus-LCE runs cache-warm at 1GB;
the 10GB rung measures the NVMe-bound transition).

## The two approved decisions (journaled A/A, 2026-10-10)

1. **Lite container member set = {m1 runs, m4 anchors, m5 chi EF, m11 sparse
   1/1024 tails}** (m7 remap and the derived m10 LF table ride along).
   Member 2 (packed per-run tails, 1.38n at 1GB) is DROPPED from the pile
   lite — it is the WARM container's member. Locate = m11 + anchored walk
   (measured ~3.1k mean LF steps on text-like pile data).
2. **Corpus-backed LCE replaces merge-emitted pftext/pfck.** The corpus is
   the canonical text store; the finish reads it read-only through the
   adapter `T[i] = REMAP[corpus[i]]` (M1: byte-identical vs the banked 1GB
   pftext, 224 remap-active bytes exercised). The finish's corpus-built
   PFCK is byte-identical to the banked merge-emitted sidecar (gated at
   1GB in G2). THE LAW is restored: no text is ever materialized to disk.

## Measured 1GB walls (packed-WM front-door gate xsa-pa-wmpack-1b, 2026-10-11:
10 chunks flat k=10, 48 threads, band=65536, window=64; the G2 unpacked run
measured 264/1538/4575 - the packing + quieter box roughly halved every wall)

| phase | wall at 1GB | rate (s/MiB) | pile projection (EXTRAPOLATED) |
|---|---|---|---|
| chunk | 205.8 s | 0.216 | 74.8 h |
| merge walk (load+anchor+cross-merge) | 725.5 s | 0.760 | 263.6 h |
| merge(v2 emission) ∥ fused finish | max(2802, 2868) = 2867.6 s | 3.005 | 1042.0 h |
| container (m5 EF + m1 runs + publish) | 59.6 s | 0.062 | 21.6 h |
| **total (concurrent pipeline)** | **~3079 s end-to-end** | **3.23** | **1401.8 h (~58.4 days)** |

The fused finish's own split at 1GB (MEASURED, this run): pfck-corpus
+ runcols-idx poll 1206.8 s, seam classes 218.6 s (packed-WM scan),
sweep 1389.0 s = repair 50.8 s + corpus LCE 1217.1 s (parallel, 48t) +
advance/witness ~121 s (residual), m5-ef 27.1 s, m1-runs 32.4 s,
publish 0.14 s. The G2-era "advance+witness ~2060 s" 25x suspect is
REFUTED: it was a subtraction artifact of the gate run (merge pacing +
bridge I/O in then-unmeasured regions). The serial machine costs ~121 s
(1.5x v1's 83 s); the sweep's binding term is the thread-scaled corpus
LCE, which is what the 96/192-thread curve attacks.

## Measured 1GB per-terms (G2, all byte-gated vs the bank)

| term | bytes at 1GB | n-units | pile (EXTRAPOLATED) |
|---|---|---|---|
| corpus (outside the scratch tree) | 1,000,000,665 | 1.000n | 1218 GiB |
| chunk set, compact pos (transient, freed as consumed) | 4.4n | 4.40n | 5360 GiB |
| merge transient `.xsc` (m2 2n + packed WM w/8·n + hs 2n + bwt 1n + ov/walk ~1.45n) | 10.20e9 du (w=30) | 10.20n | 11.57n (w=41) = 14,099 GiB |
| frag.rlebwt + meta (the only pre-lite whole file; deleted post-publish) | 1,545,900,600 | 1.546n | 1883 GiB |
| pfck scratch (corpus-built; deleted post-sweep) | 363,636,648 | 0.364n | 443 GiB |
| pieces window (64 bands in flight) | ~52 MB | 0.052n | 63 GiB |
| lite container (the product) | 467,814,822 | 0.468n | 570 GiB |
| **scratch-tree peak, output side (MEASURED timeline, du-sampled, excl. the merge's .xsc)** | **1,974,887,564** | **1.975n** | **2406 GiB** |
| whole-tree peak INCLUDING the merge transient (front-door du sample; the monolithic-merge hosting number) | 15,042,581,059 | **15.04n** | **16.41n = ~19,995 GiB** |
| whole-box (corpus + output-side tree peak) | — | 2.975n | **~3.9 TB** |

The mission's output-side bound is 2.5n; the measured tree peak is 1.975n
(G2 gate assert, `peak-monitor.log`). The merge transient is a separate
per-term with its own provenance (the charter's accounting): MEASURED
10.20n at 1GB packed (14.46n unpacked; the WM packing is the -4.25n),
11.57n = 13.8 TiB at pile (w=41); the whole tree including it peaks at
15.04n measured / 16.41n = 19.5 TiB extrapolated at pile. **The monolithic
pile merge does not fit this box** (nvme1+2+3 = 7.1 TB free): per-shard
transients cannot collapse it — m2, hs, bwt and the walk columns are
global by construction (the walk's LCE probes arbitrary cross-boundary
positions; per-side repair state must be complete before boundary
planning), and the WM is pinned whole by the two-pass emission plus the
finish's class-SA-set preads (interior SA is not carried by pieces). The
launch requires a >= 20 TiB-free scratch host (plus corpus if co-hosted,
plus the unmeasured witness-spill headroom) or landed reader-level
striping (lane item 3) on a box with >= 19.5 TiB aggregate. Fan-in does
not help the top merge (same globals at the final level); corpus dedup is
a product decision (PARKED). The dry run prints this explicitly rather
than hiding it in a constant.

The witness stream: chi = 0.283n at the 1GB FIXTURE (chi = 283,296,933);
the pile's chi/n is UNMEASURED (real Pile text, different repeat
structure). At the fixture ratio the stream is ~2758 GiB of values; it
lives in RAM up to `--wit-ram-budget` and spills beyond as sorted
varint-delta disc runs (est. <= 0.3x raw, EXTRAPOLATED — unmeasured at
spill scale; a 10GB rung with du-sampled spill telemetry would measure
it). Choose the budget from the host's free RAM: on this box (~930 GB
available) budget ~600 GB and expect ~0.5-0.9n of spill inside the 2.5n
bound (still feasible: 1.975n + spill <= 2.5n requires spill <= 0.5n —
**at the fixture ratio this is TIGHT; measure at a 10GB rung before
committing the pile launch**).

## The pile launch command line

Caps on every process (timeout + ulimit + the finish's own
`--poll-timeout`); scratch under the pile scratch tree only; the plan
journal (this file + `g5-plan/pile-dry-run.log`) is the scratch
journal's first entry. `n` = 1,307,910,802,540; threads = 48 measured
(higher thread counts are UNMEASURED — the sweep's LCE and witness
phases are thread-scaled, so the 97-day projection is a ceiling that
more threads shave, EXTRAPOLATED).

```sh
# 0) the plan (this document's dry run, journaled first)
xsa build --input /mnt/nvme2n1/erikg/pile.txt \
          --scratch /mnt/nvme2n1/erikg/extcols-scratch \
          --chunks 64 --kway 64 --threads 48 --memory-gb 700 \
          --scratch-free-pct 0 --plan-only --v2-pipeline

# 1) chunk front end (certified, compact pos mode, delete-as-you-go)
/home/erikg/.cache/xsa/<hash>/chunk_frontend /mnt/nvme2n1/erikg/pile.txt \
    64 $SCRATCH/chunks $SCRATCH/remap-256.bin

# 2) the merge, v2 emission (CROSS_V2_PIECES=1), flow window 64 bands
#    PRE-REQ: the merge transient (11.57n = 13.8 TiB at this scale; the
#    whole tree incl. it ~19.5 TiB) must fit the scratch it runs on -
#    stripes / lane item 3 / a bigger host (see above).
( ulimit -v 137438953472; exec timeout 6220800 \
    env CROSS_V2_PIECES=1 CROSS_V2_PIECE_WINDOW=64 \
    $BIN/cross_lcp_merge --tree $SCRATCH/chunks 64 1307910802540 \
        $SCRATCH/merged/frag --threads 48 --kway 64 --work $SCRATCH/mwork ) \
    > $SCRATCH/merge.log 2>&1 &

# 3) the fused finish, CONCURRENT with 2 (opens the merge's WM during the
#    pair phase; a late start fails loud on the WM poll - start it with 2)
( ulimit -v 137438953472; exec timeout 6220800 \
    $BIN/pile_finish --prefix $SCRATCH/merged/frag \
        --corpus /mnt/nvme2n1/erikg/pile.txt --remap $SCRATCH/remap-256.bin \
        --output $SCRATCH/pile.sxi2 --threads 48 --poll-timeout 86400 \
        --wit-ram-budget 644245094400 \
        --chi-expect <measured-at-1GB-rung-first> ) \
    > $SCRATCH/finish.log 2>&1 &
```

Pre-reqs and honest unknowns for the launch:
* The merge transient does not fit this box monolithically — the launch
  line above is the shape; hosting/striping it is the journaled pre-req.
* chi at pile scale is unknown: run a 10GB rung first (the banked 10GB
  rung exists for the v1 chain; the v2 chain needs its own) to measure
  chi/n, the witness-spill compression, and the tree peak with spills.
* `--kway 64` (flat, k = chunk count) is extrapolated beyond the gated
  k=10/k=16 shapes; the fan-in shape (`--kway 4`) is the banked
  shape-independent alternative (G3-gated byte-identical product).
* rlebwt dies after publish (the default; no `--keep-rlebwt` in the
  launch); band pieces are scratch, consumed and deleted band-by-band;
  no agg/ssa_t/head_sa/pftext/pfck file ever exists.
