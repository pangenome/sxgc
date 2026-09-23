## ============================================================
## CURRENT OBJECTIVE & STATE  (prepend — everything below is the
## append-only gate history, unchanged)
## ============================================================

**Objective**: suffixient arrays (χ) for fast search at HPRC v3 scale;
HPRC v2 (466 haplotypes, 1.4 Tbp, AGC archive on disk) is the development
vehicle; yeast235 (3.34 Gbp) is the unit gate (χ = 85,404,240 = 2.56% of n).

**Active rung: k=50 — GREEN (2026-09-21). Next: full-466 v2.**

**k=50 human rung GREEN**: χ(k=50) = **1,754,597,135** (n = 150.95 Gbp,
R = 2,033,460,666, 4,108 contigs; χ/n = 1.16%, **χ/r = 0.863**). Full
substrate chain + oracle verdict: 200 planted 120-mers, **17,527/17,527
occurrences byte-verified vs the AGC archive, 200/200 truths**. Chain:
prep 22 min; grlBWT 2h (artifacts reused across the resume);
TeraLCP 38 min / 56.4 GB index; χ+samples walk **10,227 s / 216.5 GB
peak** (the 2,033,460,666-run .ri4, sa_w=38, same pass); query 1,067 s /
52.6 GB. The sidecar parser learned the k=50 lesson: AGC contig names
may contain spaces (chm13 descriptions) — tab-split fields now.

**χ ≈ 0.86·r across three scales**: 0.846 (yeast, R=100.9M), 0.874 (k=10,
R=1.86B), 0.863 (k=50, R=2.03B) — three independent constructions,
±1.5%. χ growth is saturating like r: k=50 is +8% χ over k=10 for 5×
sequence. Projection for 466 (R≈2.53B): **χ ≈ 2.2 B**.

**MEM/MS rung GREEN at s200 (2026-09-21)**: TeraMS matching statistics
(the read-query seed engine for pangenome mapping) — 50 mutated 200bp
reads, MS lens **10,000/10,000 byte-exact** vs the divsufsort brute force,
all pos values positionally valid (phi-reposition ties are legitimate).
**Vendored patch #4** (charToBits N/T swap: byte-order codes assign
N=4,T=5 but charToBits mapped T=4,N=5 — corrupted MS on ANY text with
N, i.e. production human; pre-patch 9,722/10,000 lens wrong). MS scope:
uppercase ACGTN production path proven; soft-masked pattern queries need
a generic alphabet map (future patch, non-production).

**Active rung before this (k=10 rework) — GREEN (2026-09-21)**: the full
substrate chain replaces every O(n) component at human scale. TeraLCP
k=10 index: 1,206 s / 59.2 GB peak / 51.1 GB on disk (construction only;
R=1,859,825,801, n=30,151,407,545, 865 contigs). teralcp_chi: **χ(k=10)
= 1,627,063,183** (5.4% of n; χ/r = 0.87) + **v4 SA samples in the same
pass** in 6,753 s / 195.9 GB peak (pre-optimization; the optimized walk
re-gated the same outputs in **1,657 s — 4.1×**, byte-identical, 18.2M
rows/s). **Locate gate**: rindex_query v4 on the new samples reproduces
the oracle-verified v4 reference byte-for-byte; **pilot oracle verdict
GREEN: 12,034/12,034 + 200/200**.

**--slim memory trim (load side)**: skips Psi+intAtTop at deserialize
(+ two pinned sdsl format facts: int_vector headers store size IN BITS;
bit_vector/int_vector<1> headers omit the width byte) + stride-16 accel
φ lookup instead of the full 8B/run start copy — byte-identical outputs
at s200 (χ + all 11.5M samples). Walk-side aggregates stay native u64:
sdsl packed cells share 64-bit words across runs, so single-writer-per-run
races (caught twice by byte gates: corrupted bitmap via non-atomic
bit_vector RMW, then 5 wrong χ positions via packed-cell lost updates);
CAS-packed cells or fat cells are the only safe options. slim-vs-fat k=10
memory/time comparison pending a quiet box.

**Active rung before this (4a) — GREEN**: see the 4a.1–4a.3 records below.

**4a.3 χ GATE GREEN — the decisive result**: the full adopted chain
(grlBWT → TeraLCP → teralcp_chi) under the PFP-era reference convention
(reverse-of-flat, one sentinel, R=100,904,881 single-string) yields
**χ = 85,404,240 — EXACTLY the phase-0 anchor, and SET-IDENTICAL position
for position (100.00% of 85,404,240) vs `yeastA_str.txt.suff`** from the
independent PFP-era one-pass construction, modulo the single explained
offset (their .suff = (N−sa)−1 with their N one smaller: reference =
our (N−sa)−2). Chain cost at K=1: grlBWT 342 s/0.5 GB, TeraLCP 111 s/6.2 GB,
χ walk 5,637 s/9.5 GB single-threaded (K=1 kills walk parallelism — only the
reference-convention gate pays this; production BCR runs parallel).

**Production-convention anchor**: χ_BCR(yeast235) = **85,350,673** in
505 s/9.5 GB (9,901-way parallel). The BCR-vs-PFP count delta 53,567 is the
convention term (sentinel structure + contig order), calibrated at s200
(BCR 10,099,867 vs true-PFP 10,100,779 vs no-leading-sep 10,100,777);
content orientation also matters (forward-vs-rev content = 10,100,521 vs
10,100,779 at s200 — the first yeast attempt's off-by-3 was this: built with
forward per-contig content; corrected by building the stream as
'!' + reversed-line-order revlines + single '\n' sentinel).

**4a.2 sampler retired**: teralcp_chi's walks produce run-boundary SA
samples (saFirst/saLast per run) at O(r) space as a byproduct; the O(n)
rlbwt_sampler (2.2 h/69.7 GB at k=10) is retired for builds. Locate goes
φ-based: TeraIndex ms_index (LF+φ⁻¹) built at yeast in 186 s/4.9 GB;
query-side locate gate lands with the k=10 rework validation.

**4a.1 GREEN (2026-09-20)**: TeraLCP runs the full yeast235 in 3,000.9 s /
8.10 GB peak (LCP index construction 257.8 s incl. 36 s parallel LCP walk;
thresholds sweep 2,733.8 s single-threaded = 91% of wall — human projection
≈ 5 h for that phase; writing 1.6 s). Outputs: `y2.lcp_index.lcp_index`
2.46 GB; `thr.thr`/`thr.thr_pos` 504,527,365 B = 100,905,473 entries = R +
(9,901 endmarkers − 7,400 merged '\n' runs) — exact endmarker-split identity.
Differential gates: fresh crafted 30-string tiny collection (duplicates,
prefix ties, homopolymers; `bit6/teralcp_gate_tiny.py`) — BCR convention
re-derived from rlbwt_sampler.cpp semantics (multi-string sentinels $₁<…<$ₖ,
smallest, string order; reproduced grlBWT's BWT byte-identically) and TeraLCP
thresholds+positions byte-identical vs Python AND C brute (2,487/2,487); s200
mid-scale (145 Mbp, 200 strings, 11.54M split runs, the input that hung
pre-fix) — TeraLCP byte-identical vs divsufsort+Kasai brute
(`bit6/teralcp_brute_thr.c`, 18.5 s/2.0 GB) on thr AND thr_pos
(11,541,094/11,541,094). TeraLCP's endmarker-split/\n model == BCR convention
exactly (the FMD-vs-BCR sentinel risk is dead empirically).

teralcp_chi (`bit6/`): loads the lcp_index by direct sdsl deserialize
(the class members are private; layout pinned: totalLen, F, Psi, intAtTop,
Phi, PLCPsamples — and **F is the SORTED F-column, not the BWT**; run
structure comes from `--rlbwt`), per-string self-terminating LF walks
(sidebar-free: walk until landing on a bare-sentinel row), per-row LCP via
PLCP (φ-start binary search + samples−offset), per-run aggregates (topLCP,
interiorMin-excl-top, saFirst, saLast), inline scan-rs state machine (Bit-2
Lean-gated semantics; '
' → sentinel char 0; char-change logic merges split
runs). Gates: ft30 2,187/2,187 value+order byte-identical (found a convention
bug in the old triples harness: sentinel rows must be char 0 — a stray
distinct-sentinel encoding made scan-rs emit 29 spurious positions); s200
byte-identical in ALL THREE conventions (BCR 10,099,867; forward-content
single-string 10,100,521; true-PFP 10,100,779) vs independent brute force.

**Three vendored TeraTools patches, all load-bearing and now gate-validated**:
(1) `RB3_ASIZE 6→16` (soft-masked yeast = 10 symbols); (2) `_DNA_ONLY`
disabled in ropebwt3/rld0.h (hardcoded 6-symbol decoders stack-smashed the
portable path); (3) **rld block geometry** — `rld_init(asize, bbits)` must
size small blocks to the alphabet: type-1/2 counter headers need asize1×4/8
bytes = up to 17 words at asize=16, but bbits=3 gives 8-word blocks; DNA-6
fits exactly, asize=16 silently corrupts the heap and rld_rank_index spins
forever over a garbage block count (found via SIGALRM self-backtrace in a
repro probe: the 100.9M-run encode loop was innocent — 6.4 s; the spin was
in rld_enc_finish→rld_rank_index). Fix: `rld_init(RB3_ASIZE,
RB3_ASIZE > 8 ? 5 : 3)` in TeraLCP.cpp. Small inputs stay in type-0 blocks
and survive — which is why the 12-Mbp diagnostic passed pre-fix and only
real scale hung.

Operational note: pkill -x on the process name kills ALL TeraLCP instances —
the healthy yeast4 run was killed at its thresholds phase that way and cost
a clean 50-min rerun (yeast5). proc stop by id is the only safe kill.

Substrate chain: **AGC (ragc) → agc2flat --revlines → grlBWT (GPL-3,
external tool) → grlbwt2rle → TeraLCP/TeraIndex (TeraTools, MIT) → sxgc χ
layer**. The in-house PFP machinery (pscan -S, pfp_suffixient,
rindex_build, rlbwt_sampler) is the yeast-gated reference constructor and
the χ-legacy path; superseded for production, pending gates.

Rung ladder: 4a fully **GREEN** → **k=10 rework GREEN** → **MEM/MS gate
GREEN at s200 (patch #4: production-relevant N/T bug fixed)** → **k=50
GREEN (χ = 1,754,597,135; 17,527/17,527 + 200/200)** → **NEXT:
full-466 v2** (~1.44 Tbp, R≈2.53B, χ≈2.2B projected; ~1.5 days wall,
~230 GB peak RAM, 1.44 TB transient revlines on nvme — the policy item:
spend the disk, it's one-time and deleted after grlBWT) → v3.

Open risks: grlBWT needs a seekable input file (at 466 that is a 1.4 TB
revlines temp on nvme — policy decision or shard+merge); TeraTools is fresh
code ("cite: TBA") — everything must pass sxgc's differential gates
(byte-gated yeast references exist); TeraLCP's test data uses ropebwt3 FMD
(bidirectional) while ours is BCR (forward-only) — sentinel/convention
handling must be gate-checked.

---

# The Bit Ladder — serial execution plan (one bit green before the next starts)

Constraints of record: **AGC as only source; minimal RAM; near-zero scratch;
no flat text; no interim full-collection baseline.**

| Bit | Deliverable | Gate (must be green) | Status |
|---|---|---|---|
| 1 | Lean: Defs + one-pass scan + executable checks | 10/10 random texts (done) | ✅ |
| 1b | **Exhaustive** verification: all texts over {1,2}, |T| ≤ 8 — covering AND minimality | 0 failures, exhaustive | 🔄 |
| 2 | Rust `scan-rs`: consumes (c,lcp,sa) triples, emits χ-set; fork patch `--dump-triples` | **GREEN**: tiny (baa) + yeast235 **exact vs C++ pfp route** (7,501,037 byte-identical); cross-route vs one-pass differs ONLY by tie-breaking (611,520 positions, 92% overlap — smallest sets non-unique, Bit-1-established) + one-pass ±1 terminator convention | ✅ |
| 3 | **AGC-native sharded construction**: `agc2flat --groups` (metadata, 0.003 s) + `--group/--band` (targeted `get_contig`/`get_contig_range`) → temp shard → build → delete | **GREEN**: CM086560.1 AGC-extracted shard **byte-identical** to flat-mode shard + sidecar + **χ identical (78,742,930)** | ✅ |
| 3b | *(optional, re-scoped)* whole-collection χ via streamed pscan — FIFO probe failed: `mt_process_file` splits by file size (`ifstream::ate` + per-thread seek ranges); needs a streaming-mode pscan patch (spec'd, not built) | dict/parse byte-equal vs flat-mode | 📋 |
| 4 | PFP-aux emission in streamed scan: `-A` emits .suff/.lcs/.mult (0-based, lcs+1, per-char counts, sigma=max_byte+1) | **GREEN**: byte-identical vs `one-pass-build-index -t sA` on baa + CM086554.1 (244.6 MB human shard, chi=122,370,548) | ✅ |
| 5 | `-o agc` oracle: `agc_text_oracle` (dlopen `ragc-ffi` cdylib, sidecar flat-offset map, contig LRU, `get_contig_range` + CNV_NUM) wired into locate+mems | **GREEN**: 500/500 locate hits + 500/500 MEM anchors byte-identical vs flat text; **0 bytes of oracle disk** (3.3 GB AGC serves the text) | ✅ |
| 6 | full-466 run (AGC-native, streamed) | memory profile + χ + verified MEMs; no flat text ever | ⏳ |

Deferred (explicitly, with specs): toehold GAF (Phase 5), tag arrays (Phase 6),
χ_tag (RESEARCH.md). Bit 1b's general theorems (`sorry`s) stay as research-track
items; exhaustive domain checks are the Bit-1b gate.

## Differential-gate convention (from Bit 2)

Cross-route set comparison is NEVER exact (smallest suffixient sets are
non-unique — tie-breaking). Gates: (a) **same-route exact match** (scan-rs vs
the C++ route that produced the stream), (b) χ equality, (c) covering —
machine-gated in Lean (511 exhaustive). one-pass additionally emits the
terminator-run candidate (±1 entry vs Def. 9's alphabet-only extensions).

## Smoke k-scaling curve (human, measured)

| k (haplotypes) | flat | Σ.sA | Σ.lz77 | index/text | index/full-SA |
|---|---|---|---|---|---|
| 6 (3 samples) | 9.03 Gbp | 15.6 GB | 3.85 GB | 215.4% | 43.1% |
| 20 (10 samples) | 30.15 Gbp | 52.3 GB | 12.9 GB | 216.4% | 43.3% |

Small-k regime persists at 20 haplotypes; compression kicks in at much larger k
(yeast at 235 samples: 41% of full SA; HPRC v2 runs sublinear). NOTE: the
target artifact (.sA + AGC oracle) excludes lz77 entirely. smoke10: 862/865
(3 lz77-corruption stragglers — plain-text workaround as CM086560).

## Stream-convention contract (discovered in Bit 1 — binding for Bits 2/4)

The one-pass consumes `(bwt, lcp, sa)` triples of **reverse(input) + 0-sentinel**
in SA order with **0-based sdsl SA**; the scan emits `N - sa` (1-based text
positions); `eval` iterates chars **1..σ** (sentinel never emitted); chars are
remapped to 1..σ by first appearance in the reversed text (scan output is
invariant to relabeling — per-char state machine). The pfp route streams the
same colex-order triples via the PFP iterator.

## Bit 6 progress — whole-collection streamed construction + query (yeast235 gated)

- **Zero-materialization construction**: `agc2flat --reverse --stdout | pscan -S`
  (new streamed single-thread pscan mode; agc2flat emits the byte-exact mirror
  of the forward flat text). PFP outputs byte-identical to the file-based route.
- **64-bit fix**: `-n` was `atoi` — collection length 3,336,986,760 > 2^31 wrapped
  negative; every emitted position was garbage. `N` is now `uint64_t`.
- **N-from-sidecar**: build read text length from a text file that no longer
  exists; `text_length()` added across the oracle family, baseline falls back
  to it when the text file is absent (1-bit positions bug).
- **Sorted-extract build**: positions extracted in sorted order (one sequential
  AGC sweep, warm window cache), then bucketed in original colex order.
  Build 5+ h (and OOM-died) → **9 min**.
- **Whole-collection yeast235 index**: χ = 85,404,240 (= phase-0 reference);
  .sA = 341 MB (10.2% of 3.34 Gbp); `locate -o agc` 50/50 byte-verified.
- **Window-granular oracle cache** (64 KiB windows, 256 MiB byte-budget LRU,
  partial-range fills): replaces 4-slot whole-contig LRU. Query 427 → 121 ms
  (50 scattered-cold patterns); 50/50 byte-verified.
- **Reader A/B** (same archive, same 300 random 64-KiB fetches):
  ragc FFI **2.4 ms/fetch (26 MiB/s)** vs upstream C++ libagc **53-58 ms/fetch
  (1.2 MiB/s)** — upstream ~25x slower at random access and it segfaults on
  ragc-written archives. ragc backend stays.
- **Query cost structure** (oracle stats): 595 byte_at + 37 window fills per
  scattered-cold pattern; fills = AGC segment decompressions dominate. Warm
  repetitions: fills stay at 1,841 total (identical) → per-pattern drops to
  ~10-35 ms class. Batch locality = the friend. Text-access-heavy binary search
  is intrinsic to sA; the r-index toehold (LF-mapping search, no text access)
  is the designed path for the 10-20 GB target's query side.

## Rung 1 gate GREEN — r-index over the same streamed PFP, zero text access (yeast235)

- **One stream, two indexes**: the same `agc2flat --reverse --stdout | pscan -S`
  PFP stream feeds both the sA (χ = 85.4M) and now the r-index
  (`rindex_build` RLEs the streamed BWT, samples SA at run ends).
- **Query side is pure index arithmetic**: `rindex_query` = backward search +
  LF-walk locate — **no oracle, no AGC touch**, answering the cost problem the
  sA measurement exposed (37 scattered AGC decompressions per cold pattern).
- **Bug found & fixed**: `rank(c, i)` with `i` past the last c-run returned
  garbage (`lower_bound` → `end()`, OOB `csum[c][j]`); per-char sentinel totals
  appended to the prefix arrays. Symptom: 8/50 patterns reported empty search
  though all were present (trace: `rank('T', 3336986757)` → 0).
- **Gate**: R = 100,904,881 runs (3.0% of the 3.34-Gbp text — the pessimistic
  diverse-yeast regime; HPRC v2 ≈ 0.18% = 2.53B runs per WABI 2025);
  867 MiB raw layout; **5991/5991 occurrences byte-verified** vs flat text
  (full occurrence sets — sA reports 1/pattern, r-index enumerates all,
  e.g. p14 = 197 copies); 50/50 patterns match brute-force `t.count()`.
- **Full enumeration timing**: 6m20s for 50 patterns / 5,991 occurrences
  (2.7 GHz-class single core) — correctness first; query-side optimization
  (toehold sampling, `r-index-toehold/`) is the next rung.

## Rung 2 gate GREEN — layout v3: SA samples wide, packed, sized for HPRC (yeast235 gated)

- **Blocker found**: rung 1's `sa_sample` was u32 — HPRC v2 (1.4 Tbp) needs
  **41-bit** SA values; the full-466 run would have silently corrupted every
  sample past 2^32 (same bug family as the Bit 6 `atoi` overflow).
- **Target accepted (user)**: ~23 GB class for the human index; "we have to
  use more bits" — SA width goes up, not down, and that is fine.
- **Format versions** (query reads all three):
  - v1: u32 SA (broken at scale) — 9 B/run
  - v2: u64 SA, 13 B/run — 1250 MiB on yeast, gate 5991/5991 byte-verified,
    50/50 patterns
  - v3: SA packed to `bits(n)` via sdsl `int_vector` — 41 bits at HPRC scale,
    **6.125 B/run → ~15.5 GB projected** (R = 2.53 B); gate 5991/5991
    byte-verified, 50/50; yeast file 866 MiB (sa_w=32 — the v3 win
    materializes at human scale)
- **run_len u32 guard**: a single BWT run ≥ 2^32 aborts the build loudly.
- **Measured build cost** (yeast, 3.34 Gbp): RLE pass peak RAM 10.5 GB,
  wall ~10 min (PFP-stream-bound); query peak RAM 3.07 GB, full-enumeration
  6m25s for 50 patterns / 5,991 occurrences.
- **Open (later rungs)**: build-side RAM at human scale (raw vectors in RAM
  during RLE ≈ 30+ GB — needs streaming-out or the compressed write path
  built incrementally); query-side RAM at human scale (csum arrays dominate);
  EF-compressed run_len (measured H = 4.96 bits vs 32 stored — optional
  further ~2.6 B/run of headroom if we ever want deep compression).

## Rung 3 gate GREEN — out-of-core PFP machinery (disk_vector), dict scaling measured

- **Decision (user)**: go out-of-core with disk-backed arrays — essential for
  HPRC v2, not an optimization.
- **Measured dict scaling** (real HPRC subsets, pscan w=10 p=100 -t 4):
  | k | n | \|D\| | \|D\|/n | pscan peak RSS |
  |---|---|---|---|---|
  | 3 | 9.03 Gbp | 4.24 GB | 47% | 7.85 GB |
  | 10 | 30.15 Gbp | 5.68 GB | 18.8% | 10.0 GB |
  - novel dict growth only ~205 MB/sample (3→10); k=466 extrapolates to
    **|D| ≈ 99 GB** (conservative linear; real growth is sublinear).
    occ/n steady at 1.01% (avg phrase ~99 bp).
- **disk_vector<T>** (`sA/include/pfp_iterator/disk_vector.hpp`): mmap-backed
  file vector; **stable element addresses** so the pfp priority queue's raw
  pointers into `ilist` keep working; scratch files unlinked on close;
  stream-in loads for `.dict`/`.parse`. Big arrays out-of-core:
  `d, saD, isaD, lcpD` (isaD freed after load), `p, saP, isaP` (freed),
  `ilist, pos_T, s_lcp_T, poss` (transient). RAM keeps only succinct
  structures (b_d, ilist_s, both rmq's) + sacak/gsacak workspaces.
- **Gates (yeast235)**:
  - `rindex_build` → `.ri` **byte-identical** to v3; anonymous peak
    **10.5 → 2.56 GB (−4.1×)**; wall 639 s (+7%).
  - `pfp_suffixient -A` → `.suff/.lcs/.mult` **byte-identical** to the
    Bit-6 references, χ = 85,404,240; peak **10.9 → 2.96 GB**.
    (First run "differed" by exactly +1 per record: `-n` convention —
    the reference uses N = sentinel-inclusive 3,336,986,760; noted.)
- **k=466 projection with disk_vector**: anonymous ~350-450 GB (gsacak
  workspace ~2×|D|, sacak_int workspace, succinct ~45 GB, run arrays
  33 GB); scratch peak ~2.4 TB on the work mount (3.6 TB free), falling to
  ~1.9 TB after load-time unlinks; pscan -S streaming pass ~74 h
  single-threaded (the zero-materialization price).

## Rung 4 — grlBWT becomes the production r-index constructor (k=10 head-to-head)

- **Decision (user)**: the PFP r-index route is retired from production. It
  remains the χ/sA construction (exhaustively gated on yeast) and the
  reference machinery.
- **k=10 HPRC head-to-head (identical 30.15 Gbp, 10 haps)**:
  | | PFP route (out-of-core, M64) | grlBWT route |
  |---|---|---|
  | peak RAM | 147 GB (killed at 3 h 11 m, unfinished) | **5.95 GB** |
  | wall | > 3 h | **43.5 min** |
  | R | — | **1,859,825,801** (n/r = 16.2) |
- **R(k) grounding**: 1.86 B runs at k=10 vs WABI's 2.53 B at k=464 — the
  sublinear run growth is real; the 466 index lands in the projected ~15 GB.
- **grlBWT k=10 construction quirk**: it stages the output in TMPDIR and
  rename()s it — TMPDIR must share a filesystem with the output (cross-device
  rename aborts AFTER construction; the BWT was salvaged intact from /tmp).
  Fixed in the runner (TMPDIR=$W/tmp).
- **v4 pipeline pieces, all gated**: agc2flat --revlines (BCR collection
  format, rotation-cancelling sidecar formula); rlbwt_sampler (parallel
  per-string LF walks, zero text access, S = fstart+L-j run-end samples;
  712/712 vs brute force); rindex_query v4 (S - m locate; S INCREASES per LF
  step — sa_at uses sample - steps, sign caught by the yeast gate).
  Yeast AAA#0 end-to-end: 114/114 oracle byte-verified, 100/100 planted
  truths. k=10 v4 chain (sampler → query → oracle gate) in flight.

## k=10 grl-v4 chain gate GREEN (2026-09-19) — first full human-scale validation

- **Pipeline**: AGC → agc2flat --revlines → grlBWT (R=1,859,825,801; 43.5 min;
  5.95 GB) → grlbwt2rle → rlbwt_sampler (parallel LF walks, S-samples) →
  rindex_query v4 → oracle verification.
- **Gate**: 200 oracle-planted forward 120-mers; **12,034/12,034 occurrences
  byte-verified against the AGC**, 0 bad; **200/200 planted truths found**.
- **Measured costs at k=10 (30.15 Gbp)**: sampler wall 7,927 s (2.2 h),
  maxRSS 69.7 GB — the O(n) walk is the chain's least scalable stage, which
  motivates retiring it via TeraLCP's Phi+samples (rung 4a.2). Query:
  110 s for 12,034 full-occurrence enumerations, maxRSS 45.6 GB.
- **Cross-construction validation at yeast235**: grlBWT R = 100,902,972 vs
  the in-house PFP route's R = 100,904,881 — agreement within 1,909 runs
  (0.002%), the expected sentinel-convention delta. grlBWT yeast build:
  246 s wall, 1.0 GB peak RSS.
- **TeraTools integration notes (rung 4a)**: rlbwt format needs 5-byte LE run
  lengths (grlbwt2rle emits u32 — converter added); TeraLCP assumes a DNA
  6-symbol alphabet (RB3_ASIZE) — vendored patch to 16 (soft-masked yeast
  carries mixed-case; human HPRC is 6-symbol unmodified); grlBWT needs
  --tmp-dir on the output filesystem (stages + rename), else it aborts after
  construction completes (salvageable from its temp dir).

## PFP-BWT adoption: two-scale gates GREEN (2026-09-22) — v3 construction route

Motivation: grlBWT needs the flat text (1.44 TB at 466) plus ~2x-text
temporaries (~2 TB) — transient, but intolerable as a repeatable cost
("3 TB of disk is intolerable; get this right"). PFP-BWT (pfp++ parse →
r-pfbwt BWT) replaces it: construction substrate proportional to r,
no flat text needed with a streaming front-end.

- **s200 pilot GATE GREEN** (145 Mbp): rpfbwt reproduces grlBWT's BWT
  run-for-run — 11,541,090 runs, ordered identical — modulo the pinned
  convention: pfp++ pads the text end with 10 rows of internal 0x02
  (sorted first; strip) and remaps the trailing sentinel to 0x02 (map
  back). rlebwt record format = u32 LE len<<8|char with NEXT_RECORD
  escapes. Disk profile ~72 MB vs grlBWT ~2x text. rpfbwt also emits
  run-head SA samples for free. (PFP-eBWT evaluated and REJECTED: cyclic
  omega-order eBWT, wrong shape for BCR.)
- **k=10 head-to-head GATE GREEN** (30 Gbp single-string h10ss.txt,
  8 threads): **ordered run sequence identical, R = 1,859,825,860
  both constructors.** grlBWT 1h32m; pfp++/rpfbwt 3h04m total (2.0x)
  — parse 21 min was SINGLE-threaded (~24 MB/s) and rpfbwt was
  thread-capped; this is the low-repetitivity worst case (n/r=16).
  PFP scratch ~8 GB = dict 5.5 + parse 1.3 + L2 1.1 (26% of text here;
  collapses at pangenome-scale redundancy) vs grlBWT's flat input +
  temporaries. Run-head SA samples: 14.9 GB, free.
- **VERDICT (pre-set 2x rule)**: PFP-BWT adopted for v3 construction.
  Remaining engineering: streaming AGC → parse front-end (zero flat
  text; pfp++ is single-threaded — the in-house pscan -S parser is the
  parallelizable replacement), byte-alphabet ingestion (see
  ROADMAP.md), and rpfbwt thread scaling at -t 96.

## LF-walk text accessor: the .ri4 is sovereign (2026-09-23) — GATE GREEN at ft30 + s200

Claim gated: the .ri4 alone (rlbwt + C + run-end SA samples — one
self-contained artifact) serves every text access the xsa query layer
needs. No AGC, no flat text at query time. New tool: bit6/rl_text_extract
(build: g++ vs vendored sdsl-lite; same flags as teralcp_chi).

- **ft30 (3,527 B, 30 strings, 2,483 runs): 5/5 checks GREEN** —
  structural (sum/C/totals); inversion (LF from bare-sentinel row i
  reconstructs string i backward — 30/30); LF-decrement law
  sa[LF^j(e)] == sa[e]-j (69,749 steps over all run ends); v4 sample
  formula S = fsFwd + (fend - sa[run_end_row]) at 2,483/2,483 runs;
  window emission from run-end anchors (72,232 chars).
- **s200 (145 Mbp, 200 strings): structural + inversion GREEN (200/200)**,
  full text reconstructed from BWT+LF alone, 7 min single-thread.
- **LAW (new, discovered by this gate): sentinel-identity erasure.**
  grlbwt2rle remaps all k BCR sentinels to byte 0x0A, so an LF step
  *from* a string-start row (whose BWT byte is a sentinel) permutes the
  sentinel rows and violates sa[lf] = sa-1 there. Within a string the LF
  law is exact. Pattern search never steps from a sentinel row (patterns
  contain no 0x0A) — which is why locate passed 12,034/12,034; and the
  accessor contract (verify m bytes at a position inside one contig,
  windows bounded by the owning string) never crosses one either.
  First-pass ft30 CHECK3 failure was this law, not a bug: the gate was
  corrected to bound windows to the owning string (w = s - fstart[i]).
- k=10 (30 Gbp) inversion gate running in background (proc_e8b0).
Consequence recorded: the AGC demotes to build-time input; query modes
(witness / leftmost / tags / enumerate / MS) run from rlbwt + .ri4 +
chi .sA + boundary sidecar alone. The LF-walk accessor is the
text-free fallback for the chi verify clause (~100 us-class per seed
at 466: L rank steps + a short hop to the nearest run-head sample).

## Session-continuity notes (2026-09-23) — for any fresh agent/human picking this up

Repo state is always the source of truth: everything below is committed and
pushed. If a session dies mid-run, the OS processes keep running; check with
`ps aux | grep -E "teralcp_chi|rl_text_extract"`. Both chains are
stage-guarded: rerunning the script skips completed stages.

- **IN FLIGHT #1 — 466 chi walk** (`bit6/grl_v4_466.sh`, cwd
  /mnt/nvme3n1/erikg/sxgc-pilot/k466): teralcp_chi over h466rt.lcp_index,
  -t 96, ~255 GB RSS, started 2026-09-22T22:25Z. It emits, in order:
  h466.ri4 (samples), chi_h466.sA (chi positions), then the chain
  continues: patterns -> query -> AGC oracle verdict ("PILOT VERDICT").
  On completion: record chi(HPRC v2) here, law check chi vs 0.86*R
  (R=2,739,735,806), commit. h466_rl.txt (1.44 TB revlines) must be
  KEPT until iteration 2 (PFP-BWT at 466) completes — it is iteration 2's
  parse input; delete only after that gate.
- **IN FLIGHT #2 — k=10 accessor gate**: /tmp/grl_gate/rl_text_extract
  h10new2.ri4 h10_rl.txt 865 --threads 32 (30.15 Gbp, 865 contig
  strings, ~90 GB RSS, tens of minutes). Verdict goes to this ladder.
  NOTE k in .ri4 = strings (contigs), not haplotypes.
- **Gate binaries live in /tmp/grl_gate (tmpfs — wiped on reboot)**;
  rebuild from committed source with the vendored sdsl:
    SDSL=/home/erikg/TeraTools/src/thirdparty/sdsl-lite
    g++ -O2 -std=c++17 -I $SDSL/include -I $SDSL/build/include \
        bit6/rl_text_extract.cpp -o /tmp/grl_gate/rl_text_extract \
        -L $SDSL/build/lib -lsdsl -pthread
  (teralcp_chi/teralcp_ms_brute/teralcp_brute_thr: same pattern; proven
  by the 2025-09 power outage.)
- **Where things live**: /mnt/nvme3n1/erikg/sxgc-pilot/{k466,k50,k10}/
  (artifacts + chain scripts' cwd), /mnt/nvme3n1/erikg/sxgc-yeast/grl/
  (yeast), /tmp/grl_gate (gates + s200/ft30 fixtures), lean/ (Bit 1),
  bit6/ (production tools + chain scripts + vendored patch README).
- **Standing rules**: kill processes by id, NEVER pkill by name (it has
  killed a healthy run and once the issuing shell); 96-core policy
  while the 466 build runs (gates: --threads caps); grlBWT -T must
  share the output filesystem (EXDEV, 2026-09-22); every claim gets a
  byte/oracle gate before it enters this ladder; timings on this shared
  box are order-of-magnitude only — correctness and feasibility only.
- **Next rungs**: ROADMAP.md §14 (T0 -> T1: xsa veneer, chi_tags,
  names index, seed_project, syng cross-measure -> T2: iteration 2 +
  streaming front-end).

## T1 rung 1: `xsa stats` GREEN (2026-09-23) — the tool lane opens

`xsa` Rust crate scaffolded (xsa/Cargo.toml, src/main.rs, std-only).
First subcommand `stats`: reads the .ri4 v4 header (magic ISXR, n, k, R),
the rlbwt pair (heads = R, u40 lens summed = n), and the chi .sA
(filesize/8); prints n/k/r/chi, n/r, chi/r against the 0.86 law.

Gate: run against every fixture scale, all values EXACT vs this ledger:
- ft30:   n=3,527 k=30 R=2,483 chi=2,187 (matches the accessor-gate run)
- yeast:  n=3,336,986,759 R=100,902,972 chi=85,350,673 (BCR anchor,
  exact) chi/r=0.846
- s200:   R=11,541,085 chi=10,099,867 (BCR, exact); all seven chi_s200
  convention variants agree byte-for-byte in count (80.8 MB each)
- k=10:   n=30,151,407,545 k=865 R=1,859,825,801 chi=1,627,063,183
  (exact) chi/r=0.875
- k=50:   n=150,950,436,067 k=4,108 R=2,033,460,666 chi=1,754,597,135
  (exact) chi/r=0.863
New recorded fact: k50's k = 4,108 contig strings (50 haps).
Next T1 rungs: chi_tags, names index, seed_project (each gated at
yeast/ft30/s200 first, applied at k=10, then 466 when the walk lands).
