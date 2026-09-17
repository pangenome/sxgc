# sxygc — s-χ-GC: Suffixient-χ over Genome Collections

*Suffixient-array indexing of AGC collections: measured constants, scaling model,
and phase plan. All numbers below were measured on the target workstation
(256 cores, 1 TB RAM, 14 TB NVMe) unless attributed to external sources.*

---

## 1. Mission

Build and query **suffixient-array (sA) indexes** over **AGC-compressed genome
collections** with results mapped into the **sample/contig name space**, scaling
from 235 yeast strains (validated) through HPRC v2 (466 samples, 1.4 Tbp) to
collections of ~10,000 haplotypes.

Roles in the existing index stack (impg/syng syncmer sparse index, ropebwt3 FMD):
the sA is the **dense-but-tiny locate-one-occurrence / MEMs index** — complement,
not replacement.

## 2. Measured constants (yeast235: 235 strains, 9,901 contigs, 3.34 Gbp)

### 2.1 Construction

| Stage | Measured | Constant |
|---|---|---|
| AGC → flat (agc2flat) | 51 s incl. compile; ~40 s run | ~80 MB/s single-thread |
| PFP parse (pscan, 32 t) | 49 s | 68 MB/s aggregate; dictionary 3.17 M words (519 MB), parse 32.9 M phrases |
| PFP χ scan (single-thread) | ~636 s | **5.25 MB/s per core** — the bottleneck constant |
| Peak RAM (whole PFP stage) | 8.8 GB | 2.6× n for this collection; scales with dict+parse (novelty), not n |
| χ (whole collection, PFP) | 85,404,240 (2.56% of n) | `.suff` 427 MB |
| Banded indexes (975 shards, 48-way) | **236 s wall** | Σ.sA 1.23 GB + Σ.lz77 0.40 GB = **1.63 GB = 9.8% of full SA, 0.49× text** |
| Whole-collection index build | ❌ segfault | lz77 oracle: divsufsort `saidx_t=int32` → 2³¹ cap (3.34 Gbp > 2.147 Gbp) |

### 2.2 Queries (chrIV shard, 265 MB text, 37.7 MB .sA)

1,000 random 100-mers: **1000/1000 byte-verified correct**, 0.26 ms/pattern,
2.55 µs/char, 38 MB RAM. Hits mapped via `mappos.py` to `sample#contig:offset`.

### 2.3 Cross-validation

- PFP χ = one-pass χ exactly (7,501,037 for single-genome yeast; sets differ only
  by Lemma-34 tie-breaking, 92% overlap).
- ragc reads C++-AGC archives (yeast235, HPRC v2 0.6.1) bit-compatibly.
- `get_sample()` returns **numeric base encoding (0–15)** — must convert via
  `CNV_NUM` (fixed in agc2flat; forbidden-byte validation caught it).

## 3. External reference points (HPRC v2)

From "Lossless Pangenome Indexing Using Tag Arrays" (Eskandar/Paten/Sirén,
WABI 2025) on equivalent hardware:

| | HPRC v1.1 | HPRC v2.0 |
|---|---|---|
| Haplotypes / sequence | 90 / 257 Gbp | 464 / 1,317 Gbp |
| BWT runs | 3.89 B | 2.53 B (sublinear in n!) |
| Whole-genome r-index build | 5.3 h | **19 h** |
| FMD bidirectional (parallel per-chr) | 33 h | 133 h; peak 1.04 TiB |

Local prior work on this box: impg syng whole-HPRC pipeline (May 13–19, staged
+ repair passes, days wall-clock; pstep stage alone 6,719 s); ropebwt3
`human579.fmd` 30.5 GB. **Expect sA-class builds in the same order as the 19 h
r-index, not the 1.9 h stage figure.**

## 4. Scaling model

Aggregate wall-clock for the build phase, given S shards/bands kept ≥ core count:

```
wall(scan)   ≈ n / (cores × 5.25 MB/s)        # per-shard single-thread, parallel across shards
wall(parse)  ≈ n / (threads × ~2 MB/s)         # scales with threads
RAM(per job) ≈ O(band novelty), ≪ 2 GB at 2.1 GB bands   # NOT O(collection)
RAM(whole-collection χ) ≈ 200–600 GB @ 1.4 Tbp # grows with novelty accumulation
Disk         ≈ flat text + ~5–10% text temps + banded indexes (0.49× text measured)
```

Projected (banded, full node):

| Collection | n | Scan wall | RAM/job | Banded index |
|---|---|---|---|---|
| yeast235 ✅ | 3.34 Gbp | done | ≤1.5 GB | 1.63 GB |
| HPRC v2 | 1.4 Tbp | **~20 min scan + ~2 h parse ≈ 3–4 h** | ≪2 GB | ~200–700 GB |
| 10,000 hap (31 Tbp) | 31 Tbp | ~9 h wall (~2,200 CPU-h) | ≪2 GB | ~15 TB → cluster port |

**Verdict**: one big shared-memory multicore node + NVMe scales through several
thousand haplotypes with the banded architecture; band jobs port trivially to
SLURM beyond that. χ itself grows sublinearly (novelty coalescing — HPRC v2 runs
*decreased* vs v1.1 with 5× sequence).

## 5. Phase plan

### Phase 0 ✅ — yeast235 end-to-end (done)
Validated: agc2flat (CNV_NUM fix), sharding, 975/975 builds, 1000/1000 verified
mapped hits. Failure forensics: lz77 2³¹ segfault characterized.

### Phase 1 — AGC random-access oracle (`oracle-agc/`)  ← *next*
Replace the lz77 text oracle with random access served from the AGC archive via
ragc-core FFI. **Eliminates the 2³¹ blocker entirely and cuts the queryable
footprint ~10×** (no lz77 = no 12%-of-text oracle; 300–400 GB → 30–60 GB).

Spec:
- Build `ragc-core` as cdylib; C++ oracle class implementing the toolchain's
  random-access interface (`-o agc` alongside lz77/rlz/bitpacked).
- Partial extraction at segment granularity via the existing FFI
  (`ffi/segment_helpers.rs`, `test_get_part.cpp`) — **not** `get_sample` (which
  decompresses 3 Gbp contigs).
- LRU segment cache (few hundred MB); MEMs/locate access is per-read-window and
  cache-friendly. Expect slower queries than lz77 (decompress per miss) — queries
  are the cheap phase.
- Acceptance: yeast235 banded queries through the AGC oracle byte-verified
  1000/1000; then HPRC v2 3-sample smoke (Phase 2) with zero lz77 files.

### Phase 2 — HPRC v2 smoke (3-sample, then 10-sample AGC)
End-to-end on real human data: flat (19/62 Gbp), shard, banded builds with the
AGC oracle, MEMs + verified mapped positions. Fits current free disk (1.4 TB).
Acceptance: χ measured; MEMs byte-verified; name-space mapping exact; constants
refreshed for the human scaling model.

### Phase 3 — HPRC v2 full (466 samples, 1.4 Tbp)  — *disk-gated*
Prerequisites: **~4–5 TB scratch** (flat 1.4 TB + temps + banded indexes
~0.2–0.7 TB). Current free: 1.4 TB (disk 90% full) — cleanup/addition required.
Banded builds 48–96-way; χ via PFP parse (~2 h) + scan (whole-collection 74
CPU-h, or per-band ~20 min wall at full node). Acceptance: χ reported; banded
MEMs verified; comparison vs 19 h r-index and human579.fmd footprint.

### Phase 4 (optional) — whole-collection minimality
1. **M64 rebuild** (switch exists in `common.hpp`; gsacak64/divsufsort64 already
   linked) — mandatory only if n > 2³².
2. **PFP-based emission of `.sA` aux vectors** (lens/lcs/alph) — replaces the
   O(n)-RAM `one-pass-build-index`; verify vs one-pass on small collections.
   Buys whole-collection .sA (8–20 GB total) vs banded (30–60 GB); cost: 74 h
   scan wall unless the per-BWT-run-parallel scan is engineered (runs are
   independent; merge by character).

## 6. Component contracts

### agc2flat (Rust, ragc-core)
- Input: `.agc`; Output: `<out>.txt` (flat, `$`-separated, ASCII via `CNV_NUM`,
  0x00–0x02 rejected post-conversion) + `<out>.names.tsv` (`name \t start \t len`).
- Invariant: `Σ(contig lens) + #separators = flat length` (verified exact).
- TODO: parallel extraction via `Decompressor::clone_for_thread` (~1 h work, ×10).

### shard_by_contig.py / future banding
- Groups by contig part (text after 2nd `#`); shard-relative sidecars.
- Band mode (Phase 3): position-wise split keeping shards < 2.1 GB
  (yeast chr-shards ≤ 265 MB; human chr1 @464 hap ≈ 116 GB → ~55 bands).

### mappos.py
- Binary search over sidecar; offset on `$` reported as boundary (never
  misattributed); occs/mems modes.

### oracle-agc (Phase 1)
- C++ class, toolchain oracle interface (`-o agc`), ragc-core cdylib FFI,
  segment-granular partial reads, LRU cache. Must handle multi-contig access and
  report archive-relative contig identity for mapper coordination.

## 7. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| Scratch disk for Phase 3 | blocker | cleanup/add ~4–5 TB before launch |
| FFI partial-read correctness (oracle-agc) | medium | byte-verify vs flat text on smoke |
| PFP-aux emission semantics (Phase 4) | medium | differential test vs one-pass-build-index |
| Scan wall (74 CPU-h @ HPRC v2, whole-collection) | low | banded (~20 min wall) or per-run-parallel scan |
| ragc numeric-encoding regressions | low | forbidden-byte validation + CNV_NUM round-trip test |
| Disk 90% full now | blocker (P3) | Phase 2 smoke fits in 1.4 TB free |

## 8. Decision log

1. **Banded (per-contig) indexes, not whole-collection** — 2³¹ lz77 limit (measured
   segfault); banded = 975/975 success, 236 s, parallel; matches WABI 2025
   per-chromosome HPRC pattern.
2. **`-v sA -o lz77` config, not opt-sA/rlz** — N's + IUPAC in real collections
   violate DNA-only oracles; any-ASCII variant is also the minimal-space one.
3. **AGC oracle over lz77-64 rebuild** — removes the limit instead of raising it;
   10× smaller queryable footprint; reuses existing FFI surface. lz77-64 buys
   nothing AGC doesn't (RAM ~10n for whole-chr SAs is unaffordable anyway).
4. **Minimal-χ `.suff` kept as artifact** even when banded indexes are the
   queryable form (427 MB vs 1.63 GB on yeast).
5. **`$` separators per contig** — prevents cross-boundary chimeric substrings;
   `$` (0x24) legal (only 0x00–0x02 forbidden).
6. **Names from AGC headers, same-pass sidecar** — mapping can never diverge from
   the indexed text (single validation+write pass).
