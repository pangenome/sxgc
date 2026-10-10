# FAN-IN MEASUREMENT TABLE (deliverable)

The table the user asked for: walk wall and MB/s vs side size; footprint per
corpus byte; peak disc flat vs fan-in with delete-as-you-go; and the pile
projection at candidate chunk sizes. **Provenance is per cell**: `measured`
(which run), or `extrapolated` (from which measured rate). Byte-identity +
chi is the arbiter; every measurement gate ran ALONE with caps (timeout +
`ulimit -v`).

Corpus bytes `n` are the fixture's snap length: 100 MB fixture n=100 000 503;
1 GB fixture n=1 000 000 665. `x n` means bytes / n.

## 1. Walk wall & throughput vs k and side size  (the chunk-size-deciding curve)

Measured, 100 MB fixture (16 chunks, 6.25 MiB sides), one binary a7ec3df9.
"cross-merge wall" is the `CROSS_PHASE cross-merge` total for the level; MB/s
= corpus bytes walked / wall.

| shape / level        | k (interleave) | side size | cross-merge wall | walk MB/s | pool hit rate |
|----------------------|----------------|-----------|------------------|-----------|---------------|
| flat k=16 (1 level)  | 16             | 6.25 MiB  | 37.68 s          | 2.65      | 99.97%        |
| fan-in k=4 L0 (4 grp)| 4              | 6.25 MiB  | 16.62 s          | 6.02      | 99.95%        |
| fan-in k=4 L1 (final)| 4              | 25 MiB    | 20.73 s          | 4.82      | 99.94%        |
| fan-in k=2 L0        | 2              | 6.25 MiB  | 7.95 s           | 12.58     | >99.9%        |
| fan-in k=2 L3 (final)| 2              | 50 MiB    | 8.66 s           | 11.55     | >99.9%        |

The walk is **interleave-depth-bound, not side-size-bound, at 100 MB**: k=2 is
~4.7x the per-byte throughput of k=16 at the SAME side size (6.25 MiB). Side
size is a secondary lever (k=4: 6.25 MiB sides 6.0 MB/s vs 25 MiB sides
4.8 MB/s). All pool hit rates are >99.9% here.

1 GB fixture, same binary (a7ec3df9), measured. **Same-binary only** — the
earlier k=16 row (57% pool, 1597 s) came from the SXP4/LRU binary lineage and
must NOT be compared against these; it is being re-measured on this binary
below.

| shape / level        | k | side size | cross-merge wall | walk MB/s | M2 pool hit rate |
|----------------------|---|-----------|------------------|-----------|------------------|
| flat k=10 (1 level)  | 10| 100 MiB   | 607.4 s          | 1.65      | 98.5%            |
| fan-in k=4 L0 grp0   | 4 | 400 MiB   | 296.9 s          | 1.35      | 64%              |
| fan-in k=4 L0 grp1   | 4 | 400 MiB   | 322.1 s          | 1.24      | 64%              |
| fan-in k=4 L0 grp2   | 2 | 200 MiB   | 21.8 s           | 9.17      | (read from log)  |
| flat k=16            | 16| 62.5 MiB  | (curve sweep)    |           | (curve sweep)    |
| flat k=20            | 20| 50 MiB    | (curve sweep)    |           | (curve sweep)    |
| flat k=5             | 5 | 200 MiB   | (curve sweep)    |           | (curve sweep)    |
| flat k=2             | 2 | 500 MiB   | (curve sweep)    |           | (curve sweep)    |

The curve's driver is **not established** by the k=16 57% figure (cross-binary
trap). The same-binary sweep (`curve-1b.sh`, all at 1 GB on a7ec3df9) is
separating (side size) vs (k) vs (total live bytes): k=10@100 MiB [done],
k=16@62.5 MiB, k=20@50 MiB, k=5@200 MiB, k=2@500 MiB. The k=4 fan-in rows
already hint that WORKING SET (k x side structure bytes), not k alone, is the
lever: k=4 at 400 MiB sides is 1.3 MB/s but the k=2 sub-group at 200 MiB sides
is 9.2 MB/s. Curve stated with (side size, k, total live bytes) once the sweep
lands; rows go to `curve-1b.results`.

## 1b. Peak disc at 1 GB (measured)

| shape     | mode  | peak merge disc | end tree | merges | 10/10 bytes | chi |
|-----------|-------|-----------------|----------|--------|-------------|-----|
| flat k=10 | dense | 30.5 GB (30.5 n) | 37.7 GB | 1 | PASS | exact |
| fan-in k=4| dense | (in flight)      |          | 2 | (in flight) | |

## 2. Footprint per corpus byte (100 MB, measured, `du -sb` of each dir)

| term                | dense (SXP3) | compact (SXP4) | note |
|---------------------|--------------|----------------|------|
| chunk set           | 8.807 n      | 4.094 n        | pos column is the whole difference (8.0n -> 3.29n) |
| merged outputs      | 9.110 n      | 9.110 n        | mode-independent (rlebwt 1.546n + ssa 3.092n + ssa_t 3.092n + pftext 1.0n + pfck 0.381n) |
| finish artifacts    | 21.073 n     | 21.073 n       | mode-independent: **agg 12.367n** + ri4 3.237n + head_sa 3.092n + sA 2.377n |
| flat end tree       | 39.0 n       | 34.3 n         | chunks + merged + finish |
| fan-in end tree     | 30.2 n       | 30.2 n         | chunks + intermediates consumed; only merged + finish live |

`frag.agg` alone is **12.37 n** — the largest single output term (see
`FRAG_AGG_SURVEY.md`).

## 3. Peak disc: flat vs fan-in, delete-as-you-go  (100 MB, measured)

`MERGE_PEAK_DISC_USED_BYTES` polls the scratch tree every 2 s through the
merge, so it catches the mid-merge peak a phase-boundary `df` misses.

| shape      | mode    | peak merge disc (x n) | end tree (x n) | merges |
|------------|---------|-----------------------|----------------|--------|
| flat k=16  | dense   | 30.54 n               | 39.0 n         | 1      |
| fan-in k=4 | dense   | 29.49 n               | 30.2 n         | 2      |
| fan-in k=2 | dense   | 29.10 n               | 30.2 n         | 4      |
| flat k=16  | compact | 24.41 n               | 34.3 n         | 1      |
| fan-in k=4 | compact | 24.80 n               | 30.2 n         | 2      |
| fan-in k=2 | compact | 26.10 n               | 30.2 n         | 4      |

At 100 MB the chunk set (8.8n dense / 4.1n compact) is small versus the
merge transient (~21n), so flat and fan-in peaks are near-equal; the
delete-as-you-go win shows up in the **end tree** (fan-in leaves only merged +
finish: 30.2 n vs flat 39.0 n). The peak gap opens at 1 GB (the chunk set is
the same fraction but the absolute transient dominates differently) and at
pile, where the chunk set is 4-9 x 1.31 TB.

## 4. Pile projection (1.31 TB corpus) — EXTRAPOLATED

Arithmetic from the 100 MB measured ratios; walls use the 100 MB walk rate and
are **optimistic / not yet calibrated at scale**. Levels and merge counts are
exact arithmetic for the fan-in shape at k=4.

| chunk size | chunks | fan-in k=4 levels (count -> ... -> 1) | merges | peak disc dense (x corpus) | peak disc compact | flat shape |
|------------|--------|----------------------------------------|--------|----------------------------|-------------------|------------|
| 100 MiB    | 12494  | 12494->3124->781->196->49->13->4->1 (7) | 4168   | ~30 n ~ 39 TB              | ~25 n ~ 33 TB     | DISC_FAIL (~48n) |
| 250 MiB    | 4998   | 4998->1250->313->79->20->5->2->1 (7)    | 1670   | ~30 n ~ 39 TB              | ~25 n ~ 33 TB     | DISC_FAIL |
| 1 GiB      | 1250   | 1250->313->79->20->5->2->1 (6)          | 420    | ~30 n ~ 39 TB              | ~25 n ~ 33 TB     | DISC_FAIL |

Even the fan-in shape is a **disc miss at pile**: peak ~25-30 n = 33-39 TB vs
~1.8 TB free. The peak is dominated by the level-0 leaf set (the whole chunk
set must exist before level 0) plus one group's merge transient. The levers
that actually fit the pile are (a) `frag.agg` compaction / streamed finish
(12 n of the 21 n finish term) and (b) a smaller chunk set (compact pos), not
the merge topology alone. This is called out as a decision for the supervisor
rather than silently changed.
