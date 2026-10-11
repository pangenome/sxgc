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

Verdict it prints: **FEASIBLE (v2 output side): scratch-tree peak ~2406 GB
(1.975n, bound 2.5n) fits 2847 GB free; whole-box (corpus + tree) ~3624 GB
= 2.975n** — 3624 GiB = **~3.9 TB whole-box** (decimal). Wall projection at
48 threads: **2338 h ~ 97.4 days** end-to-end.

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

## Measured 1GB walls (G2: 10 chunks flat k=10, 48 threads, band=65536, window=64)

| phase | wall at 1GB | rate (s/MiB) | pile projection (EXTRAPOLATED) |
|---|---|---|---|
| chunk (64-chunk shape: 264s at 10 chunks) | 264 s | 0.277 | 95.9 h |
| merge walk (load+anchor+cross-merge) | 1538 s | 1.613 | 558.9 h |
| merge(v2 emission) ∥ fused finish | max(4470, 4575) = 4575 s | 4.796 | 1662.1 h |
| container (m5 EF + m1 runs + publish) | 60 s | 0.063 | 21.8 h |
| **total (concurrent pipeline)** | **6521 s end-to-end** | **6.84** | **2338.4 h (~97.4 days)** |

The fused finish's own split at 1GB (MEASURED): corpus PFCK build 7.3 s,
seam classes 202 s (one 8n WM sequential scan + the 30-class rebuild),
sweep 4306 s (repair stream 1276 s + corpus LCE 967 s + witness/advance),
m5-ef 27 s, m1-runs 32 s, publish 0.2 s. The v1 chain at the same fixture
needed 332 s (endpoints) + 1114 s (slim) + 83 s (sweep) = 1529 s of finish
side — the fused finish pays ~2.8x wall for eliminating every whole-column
artifact; the concurrency hides it behind the emission (the pipeline's
binding phase is max(emit, finish), not their sum).

## Measured 1GB per-terms (G2, all byte-gated vs the bank)

| term | bytes at 1GB | n-units | pile (EXTRAPOLATED) |
|---|---|---|---|
| corpus (outside the scratch tree) | 1,000,000,665 | 1.000n | 1218 GiB |
| chunk set, compact pos (transient, freed as consumed) | 4.4n | 4.40n | 5360 GiB |
| merge transient `.xsc` (m2 2n + WM 8n + bwt/hash/ov ~4.5n) | 14.46e9 du peak | 14.46n | 17614 GiB |
| frag.rlebwt + meta (the only pre-lite whole file; deleted post-publish) | 1,545,900,600 | 1.546n | 1883 GiB |
| pfck scratch (corpus-built; deleted post-sweep) | 363,636,648 | 0.364n | 443 GiB |
| pieces window (64 bands in flight) | ~52 MB | 0.052n | 63 GiB |
| lite container (the product) | 467,814,822 | 0.468n | 570 GiB |
| **scratch-tree peak (MEASURED timeline, du-sampled)** | **1,974,887,564** | **1.975n** | **2406 GiB** |
| whole-box (corpus + tree peak) | — | 2.975n | **~3.9 TB** |

The mission's output-side bound is 2.5n; the measured tree peak is 1.975n
(G2 gate assert, `peak-monitor.log`). The merge transient is a separate
per-term with its own provenance (the charter's accounting): a monolithic
final merge needs ~17.6 TiB of scratch at pile scale — this box (nvme1+2+3
= 7.1 TB free total) cannot host it monolithically; the launch requires
either reader-level striping (lane item 3, space accounted, I/O split not
landed) or a scratch host with >= 19 TB free. The dry run prints this
explicitly rather than hiding it in a constant.

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
#    PRE-REQ: the merge transient (~17.6 TiB at this scale) must fit the
#    scratch it runs on - stripes / lane item 3 / a bigger host (see above).
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
