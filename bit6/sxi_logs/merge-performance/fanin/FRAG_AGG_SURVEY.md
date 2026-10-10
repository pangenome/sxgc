# FRAG.AGG SURVEY (item 5): is the 12n finish term compactable or streamable?

## What it is

`finish/frag.agg` is the per-run aggregates sidecar produced by the slim dump
(`chi_rspace_dump --slim`) and consumed only by the chi/sA sweep
(`xsa chi-rspace --stream-agg`, `xsa/src/main.rs`). Format (producer
`bit6/chi_rspace_dump.cpp` line 25, reader `main.rs` line ~1075):

```
"CRA1" u32 magic | u64 R | R*4 u64 LE column-major:
   topLCP[]  saFirst[]  saLast[]  interiorMin[]   (u64::MAX = INF)
```

So the size is fixed and exact: **32 bytes per run + 12**. Measured:

| fixture | R | .agg bytes | bytes / n |
|---------|---|------------|-----------|
| 100 MB  | 38 647 515  | 1 236 720 492 | **12.37 n** |
| 1 GB    | 368 036 609 | 11 777 171 500 | **11.78 n** |
| pile (extrapolated, R ~ 0.28 n) | ~3.7e8 | ~11.8 TB | ~9-12 n |

It is the single largest output term (the whole finish is 21.07 n; agg is
12.37 n of it). It is dense run-space data, one record per SXCR run.

## Value ranges (measured; 1e6-run head sample of the 100 MB dense .agg)

| column      | range in sample        | structure |
|-------------|------------------------|-----------|
| topLCP      | 0 .. 25 797            | LCP of the run's top row |
| saFirst     | 342 .. 100 000 502     | SA value (text position < n) of the run head |
| saLast      | 351 .. 100 000 502     | SA value of the run tail (== the .ri4 per-run SA sample, see H466 handoff) |
| interiorMin | 0 .. 32 617, 67% INF   | running interior LCP min; INF for single-row runs |

The columns are **narrow relative to u64**: text positions need
`ceil(log2 n)` bits (27 at 100 MB, 30 at 1 GB, ~31 at pile); LCPs are bounded
by the maximum LCE (== max_lce of the merge, 1984 at 1 GB), with one INF
sentinel. They do NOT delta-compact like SXCR v3 (25 B/run -> 2 B/run): the
SA columns are non-monotone full positions, so no run-delta trick applies.

## Options, cheapest first

1. **Drop the redundant `saLast` column** (saves 8 n, 25% of .agg). The H466
   handoff states `saLast == the .ri4 v4 per-run SA sample`, and the sweep
   already opens the .ri4 run/char stream. If that identity holds, the column
   is redundant and the sweep can read it from .ri4. Cheap in disc; needs the
   identity verified by parsing .ri4, then a code change + a re-gate (the
   .sA + chi must stay byte-exact; the .agg bytes themselves change).

2. **Bit-pack the four columns** (32 B/run -> ~2.5x). Fixed-width u(bits) per
   column: saFirst/saLast in `ceil(log2 n)` = 31 bits at pile; topLCP/
   interiorMin in `ceil(log2 max_lce)` or a varint with an INF sentinel.
   ~31+31+20+20 = 102 bits = 12.75 B/run => ~2.5x, i.e. 12.4 n -> ~4.9 n.
   Needs a new magic + reader + writer + re-gate.

3. **Stream it and never let it land whole** (removes the 12 n from PEAK
   disc). The sweep already reads the four columns as **four independent
   sequential streams** (`main.rs`: one `BufReader<File>` per column, seek to
   `12 + field*R*8`), one run at a time — no random access. So the finish can
   be fused (slim -> sweep over a bounded band ring buffer) or the four
   columns written as separate files and each deleted as its stream drains.
   This leaves the *peak* finish footprint at ~ (merged + sA + one band)
   instead of merged + 12 n + sA. It only helps if `.agg` is an intermediate
   rather than a shipped artifact.

## What was done / recommendation

Nothing was implemented here. Changing the `.agg` on-disk bytes breaks the
absolute byte-identity gate the whole lane rests on (all 8/8 gates compare
`finish/frag.agg` to the bank), and choice (3) depends on whether `.agg` must
be shipped — a product decision, not a lane decision. Recommendation to the
supervisor: decide `.agg`-is-shipped vs `.agg`-is-intermediate. If
intermediate, do (3) (removes 12 n from peak finish, the biggest single disc
term). If shipped, do (1) + (2) (verify the saLast identity, then bit-pack),
re-gated on byte-identical `.sA` + exact chi. All three are ~a day of gated
work each, not a cheap safe edit; none was attempted under this wall.
