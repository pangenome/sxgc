# CORPUS-SCALE OUTPUT SURVEY (item 5, critical path): can the pile fit?

Steering reframe: the 1 GB fan-in k=4 merge peak disc came out **30.5 n — the
same as flat k=10's 30.5 n**. Delete-as-you-go bounds the *inputs* (chunks and
mwork intermediates go to zero, verified), but the peak is set by
corpus-scale **outputs** that are still fully materialized. So the pile's
launchability is decided here, by whether those outputs can be deduplicated,
compacted, and streamed — not by the merge topology.

Measured on the current binary (a7ec3df9) at 1 GB (`gate-1b-flat10-d`,
n=1 000 000 665, R=368 036 609, R/n=0.368) and 100 MB for the second scale.
`x n` = bytes / corpus bytes.

## 1. Per-term table (current → proposed)

| term | current n (100MB / 1GB) | structure (measured) | proposed form | projected n | pile bytes (1.31 TB) | gate |
|------|-------------------------|----------------------|---------------|-------------|----------------------|------|
| **agg** | 12.37 / 11.78 | CRA1: 32 B/run = 4 u64 (topLCP, saFirst, saLast, interiorMin) | drop saFirst (= head SA, redundant) + saLast (= tail SA, redundant); keep topLCP+interiorMin; **stream banded** (slim writes `agg.partial`, sweep reads 4 column streams) | 0 live (streamed) .. 2.9 | 0 .. 3.8 | sweep `.sA` + chi byte-exact |
| **ri4** | 3.24 / 3.22 | header(2080) + R×c(1B) + R×len(4B) + R×tailSA packed @ w=30 b | run c/len already in `rlebwt` (drop, derive); keep packed tail SA; share the SA with `ssa` | ~1.38 | ~1.8 | endpoints + sweep byte-exact |
| **head_sa** | 3.09 / 2.94 | R×u64 head SA (== `ssa[2:]` == `agg.saFirst`) | **DELETE** — third redundant copy of head SA | 0 | 0 | endpoints output byte-exact after reader switch |
| **ssa** | 3.09 / 2.94 | 8 B header + R×u64 head SA (+2 sentinels) | pack to w=30 b/run (canonical head SA) | ~1.38 | ~1.8 | merge output byte-exact |
| **ssa_t** | 3.09 / 2.94 | R×u64 tail SA (== `agg.saLast`) | **DELETE** — redundant with `ri4`'s packed tail SA | 0 | 0 | merge output byte-exact |
| **sA** | 2.38 / 2.27 | chi × u64 witness positions (chi numerics = count) | pack positions to w=30 b/run | ~1.06 | ~1.4 | sweep `.sA` byte-exact |
| **rlebwt** | 1.55 / 1.47 | run-length BWT (char+len runs) — **the product** | keep dense (already RLE) | 1.47 | 1.93 | merge output byte-exact |
| **pftext** | 1.00 / 1.00 | BWT text (slim --adopt input) | **STREAM/DELETE** after slim adopts it | 0 | 0 | slim output byte-exact |
| **pfck** | 0.38 / 0.36 | checkpoint sidecar for pftext | **STREAM/DELETE** with pftext | 0 | 0 | slim output byte-exact |
| **rlebwt.meta** | ~0 | tiny header | keep | 0 | ~0 | merge output byte-exact |

Current total ≈ **30.9 n ≈ 40 TB** at pile (matches the standing ~39 TB
DISC_FAIL). Note the redundancy is real and verified at 1 GB: per-run **head
SA is stored three times** (`agg.saFirst` == `head_sa` == `ssa[2:]`),
per-run **tail SA twice dense plus once packed** (`agg.saLast` == `ssa_t[2:]`,
and `ri4` packs n-1-tail at 30 b), and the **run char/len structure twice**
(`rlebwt` and `ri4`).

## 2. Achievable peak (arithmetic, provenance marked)

"Streamed" terms (agg, pftext, pfck) contribute only a **window** if the
finish is fused/banded (the `agg.partial` machinery already writes incrementally;
the sweep already reads the 4 columns as independent sequential streams — see
`FRAG_AGG_SURVEY.md` §Options 3).

| scenario | live output set (n) | pile bytes |
|----------|---------------------|------------|
| current | 30.9 | 40.5 TB |
| dedup only (drop head_sa, ssa_t; trim agg saFirst/saLast) | ~13.2 | ~17 TB |
| dedup + bit-pack (ssa, ri4-tail, sA, agg LCPs @30/24 b) | ~8.0 | ~10.5 TB |
| + stream pftext/pfck/agg (window only) | ~5.0 | ~6.5 TB |
| ship the product only (`rlebwt` + a lite index) | 1.47 .. 2.6 | 1.9 .. 3.4 TB |

The steering's target (corpus 1.3 TB + window + lite container ~0.5 TB) is
reachable **only if the shipped container is basically `rlebwt` + a small
index**: even full dedup+compaction+streaming lands at ~5-8 n = 6.5-10.5 TB,
still a 4-6x DISC miss versus ~1.8 TB free. `rlebwt` alone (1.47 n ≈ 1.9 TB) is
at the free-space boundary.

## 3. What this means / decision required

The pile's shape is **product-bound, not merge-bound**: after dedup + compaction
+ streaming the merge/inputs are already tiny, but the outputs (agg head/tail
SA copies, ssa/ssa_t, ri4 run+SA, sA) are still ~5-8 n. The key question —
**which of `agg / ssa_t / head_sa / ri4 / sA` must be SHIPPED versus only
needed to compute `rlebwt` + a witness set** — is a product decision, not a
lane decision. Everything above is a *proposal*; none of it was implemented
(any `.agg`/`ssa`/`ri4` byte change breaks the absolute byte-identity gate and
needs a reader switch + a re-gate on byte-identical `.sA` + exact chi).

Recommendation to the supervisor: (1) decide the shipped-container contract;
(2) if it is "rlebwt + a lite index", the cheapest first step is delete
`head_sa` and `ssa_t` and trim `agg` to `topLCP`/`interiorMin` (dedup only,
~13 n), re-gated on the existing artifacts; (3) bit-packing and the fused
banded finish are the next tier. The k /side-size sweep (`curve-1b.sh`) is
**queued but not launched** — per steering it reports after this.

## Appendix: the .agg sub-survey (format + value ranges)

`finish/frag.agg` = `"CRA1" u32 | u64 R | 4 × R u64` = 32 B/run exactly
(12.37 n @100 MB, 11.78 n @1 GB, the single largest output term). Sample
(1e6-run head of the 100 MB dense agg): topLCP 0..25797; saFirst/saLast text
positions < n; interiorMin 0..32617 with 67% `u64::MAX` (INF, single-row
runs). Consumed only by `xsa chi-rspace --stream-agg`, which reads the four
columns as four independent sequential `BufReader` streams (no random access),
so it can be streamed band-by-band. Values are non-monotone full positions —
they do **not** delta-compact like SXCR v3 (25 B/run -> 2 B/run); the realistic
win is bit-packing (~2.5x) plus the dedup above.
