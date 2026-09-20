# sxgc — Suffixient arrays for fast search at HPRC v3 scale

*Objective: build and query **suffixient-array (χ) indexes** at HPRC v3
scale. HPRC v2 (466 haplotypes, 1.4 Tbp, one AGC archive on disk) is the
development vehicle; yeast235 (235 strains, 3.34 Gbp) is the unit gate.
The architecture is a substrate of adopted tools with sxgc's own χ layers
on top. All numbers measured on the target workstation (256 cores, 1 TB RAM,
14 TB NVMe) unless attributed externally. Living status document — the
gate-level record is `BIT_LADDER.md`; this file describes the architecture
and the plan.*

---

## 1. Mission and capability contract

Build and query **suffixient-array (χ) indexes** at HPRC v3 scale, working
from the **AGC archive** as the only source of sequence, with all results
mapped into the **`sample#contig:offset`** namespace. HPRC v2 (466
haplotypes, 1.4 Tbp) is the on-disk development vehicle; yeast235 (3.34 Gbp)
is the unit gate. The suffixient array itself — the minimal-χ definition
(Lean-proved covering/minimality), χ as a repetitiveness measure, χ_tag for
graphs — is the project's novel contribution; everything else is substrate.

**Two query capabilities, two layers:**

| Layer | Answers | Text access | Artifact size (HPRC v2 proj.) |
|---|---|---|---|
| **sA + AGC oracle** | **all MEMs + one anchor each** | probes the text via ragc FFI, window-cached, served from the 3.3 GB archive | sA ≈ 10% of text; oracle = the archive itself |
| **r-index / φ-locate** | **all occurrences** (exact enumeration) | zero text access — pure index arithmetic (LF-walk / φ steps) | scales with runs (~15.5 GB at 2.53 B runs, v3 layout) |

The sA's binary search intrinsically touches the text (~37 scattered AGC
window-fetches per cold pattern at collection scale, ~121 ms); all-occurrence
enumeration is the r-index/φ side. The hybrid (sA finds MEMs fast, feeds an
anchor to the φ/r-index side as toehold) composes both.

## 2. Architecture — the substrate chain

The sequence lives in exactly two forms: **compressed inside the AGC**, or
**transient in a stream**. Everything else is derived structure. The
production substrate is adopted tooling; sxgc's own layers are `agc2flat`,
`grlbwt2rle`, and the χ layer on top.

```
AGC archive (ragc, Rust)                  ── only sequence source
 │  ragc random access (~26 MiB/s; 2.4 ms/64 KiB)
 ▼
agc2flat --revlines [--samples K.txt]     ── sxgc's own streaming adapter
 │  one reversed contig per line ('\n' sentinel) = BCR collection format
 │  (grlBWT's input convention); same-pass sidecar with forward flat
 │  offsets; haplotype-subset CLI preserving archive order, aborting
 │  loudly on unknown names
 ▼
grlBWT                                    ── adopted, GPL-3, external tool
 │  semi-external BCR-BWT construction (Díaz-Domínguez & Navarro, CPM 2022);
 │  grammar+run-length compressed intermediates. Quirk: stages output in
 │  TMPDIR and renames — TMPDIR must share a filesystem with the output.
 ▼
grlbwt2rle                                ── sxgc's own glue
 │  runs as .syms/.len; renamed to .bwt.heads/.bwt.len — directly TeraLCP's
 │  rlbwt input format
 ▼
TeraTools (UCF S. Zhang Lab, MIT)         ── adopted RLBWT→structures substrate
 │  TeraLCP: LF, ψ, φ, φ⁻¹, LCP, PLCP and samples from the RLBWT in O(r)
 │  space, O(n) time, parallel; "Phi+samples" phase; --thr-pfp mode emits
 │  pfp-thresholds-style 5-byte threshold files.
 │  TeraIndex adds LF + inverse-φ → matching-statistics index;
 │  TeraMEM/TeraMS do MEMs and matching statistics.
 ▼
sxgc χ layer                              ── the product
    one-pass suffixient scan over threshold/LCP structures → minimal-χ set
    (.sA); mappos → sample#contig:offset; queries via the AGC text oracle
    (-o agc, 64 KiB window LRU); anchor→φ-locate hybrid for
    all-occurrence enumeration
```

### Why this shape (constraints → consequences)

| Constraint | Consequence |
|---|---|
| AGC as only source | streaming adapters over the archive; the AGC text oracle serves sA queries directly from the 3.3 GB archive |
| RAM budget at 466 haps | semi-external BCR construction (grlBWT: 5.95 GB peak at k=10); TeraLCP works in O(r) space from the RLBWT |
| RLBWT as the shared root | one construction feeds LCP/thresholds (χ layer) and φ/samples (locate/enumeration) — no full SA ever materialized |
| reproducible subset science | `agc2flat --samples` (archive order preserved, loud unknown-name abort) + staged pilots |
| everything must pass differential gates | the in-house PFP machinery stays as the byte-gated yeast reference (see decision 13) |

## 3. Measured constants

### 3.1 Construction (yeast235: 3.34 Gbp, unless noted)

| Stage | Measured |
|---|---|
| χ (whole collection) | 85,404,240 (2.56% of n); `.sA` = 341 MB (10.2%) |
| BWT runs | 100,904,881 (3.0% of n — diverse-yeast pessimistic; HPRC v2 = 0.18%) |
| grlBWT (k=10 HPRC, 30.15 Gbp) | R = 1,859,825,801 runs; **43.5 min wall; 5.95 GB peak RAM** |
| PFP r-index route (k=10, same input) | 147 GB / >3 h unfinished — **retired from production** (decision 13) |
| pscan -S χ scan rate (legacy route) | 5.25 MB/s per core |
| ragc random access | 2.4 ms per 64 KiB fetch (26 MiB/s); **~25× faster than upstream C++ libagc**, which also segfaults on ragc-written archives |

### 3.2 TeraLCP reference point (external)

| | |
|---|---|
| human472 (HPRC-class), TeraLCP README | **172 GiB peak** vs ~2 TB for prior tools |

### 3.3 Queries (yeast235 whole-collection)

| | |
|---|---|
| sA `locate -o agc` | 50/50 byte-verified; 121 ms/pattern scattered-cold (427 ms before window cache); warm class 10–35 ms |
| r-index full enumeration (legacy route) | 5991/5991 occurrences byte-verified, 50/50 vs brute force; 6m20 s for 50 patterns / 5,991 occs — correctness first, query-side optimization is open |

### 3.4 Human dict scaling (measured on real HPRC subsets, w=10 p=100 — legacy PFP route)

| k (haplotypes) | n | \|D\| | \|D\|/n | pscan peak RSS |
|---|---|---|---|---|
| 3 | 9.03 Gbp | 4.24 GB | 47% | 7.85 GB |
| 10 | 30.15 Gbp | 5.68 GB | 18.8% | 10.0 GB |
| 466 (linear extrapolation) | 1.4 Tbp | **~99 GB** | ~7% | ~200 GB |

Novel dictionary growth is only **~205 MB/sample** (3→10) and sublinear;
occ/n steady at 1.01% (avg phrase ~99 bp).

### 3.5 k=466 projections (legacy out-of-core PFP design; superseded for production, kept as reference)

| Resource | Projection |
|---|---|
| Anonymous RAM (workspaces + succinct + run arrays) | **~350–450 GB** |
| Scratch on work mount | peak ~2.4 TB → ~1.9 TB after load-time unlinks |
| pscan -S streaming pass | **~74 h single-threaded** |
| `.ri` artifact | **~15.5 GB** (v3 layout) |
| Query-side RAM | ~60–70 GB (per-run csum/cruns materialization) — fits, optimization open |

Note: the substrate chain replaces these projections for production — grlBWT
peak at k=10 is 5.95 GB (vs 147 GB for the PFP route on the same input), and
TeraLCP's human472 reference is 172 GiB. The 466-scale substrate numbers
(grlBWT wall/RAM, the 1.4 TB revlines temp question) are exactly what rung 4a
and its successors must measure.

### 3.6 External reference points (WABI 2025, Eskandar/Paten/Sirén)

| | HPRC v1.1 | HPRC v2.0 |
|---|---|---|
| Sequence | 257 Gbp | 1,317 Gbp |
| BWT runs | 3.89 B | 2.53 B |
| Whole-genome r-index build | 5.3 h | 19 h |
| FMD bidirectional | — | 133 h; peak 1.04 TiB |

## 4. Component contracts

### agc2flat (Rust, ragc-core) — sxgc's own
- Modes: whole-collection; `--group <contig>` (+`--band S:E`); `--groups`
  (metadata); `--reverse --stdout` (legacy build stream, **validated
  byte-exact mirror**); `--revlines` (**BCR collection format**: one reversed
  contig per line, `'\n'` as the sentinel — grlBWT's input convention; a
  same-pass sidecar carries forward flat offsets); `--samples <file>`
  (restrict to listed AGC sample names — one per line, leading-`#` comments;
  archive order preserved; unknown names abort loudly; applies to all modes).
- Sidecar invariant: written in the same pass as the stream (metadata lengths
  can disagree with decompressed bytes — sidecar is ground truth);
  `cname \t fstart \t len` in forward flat coordinates.
- `CNV_NUM` conversion post-numeric-decode; 0x00–0x02 rejected.

### grlBWT — adopted, GPL-3, external tool (not linked)
- Semi-external BCR-BWT construction (Díaz-Domínguez & Navarro, CPM 2022);
  grammar+run-length compressed intermediates. Input: a seekable collection
  file in BCR format (`agc2flat --revlines`).
- Measured at k=10 HPRC (30.15 Gbp): R = 1,859,825,801; 43.5 min wall;
  5.95 GB peak RAM.
- Quirk: stages output in TMPDIR and renames — **TMPDIR must share a
  filesystem with the output** (cross-device rename aborts after
  construction).

### grlbwt2rle — sxgc's own glue
- Emits runs as `.syms`/`.len`; renamed to `.bwt.heads`/`.bwt.len` these are
  directly **TeraLCP's rlbwt input format**.

### TeraLCP / TeraIndex (TeraTools, MIT) — adopted
- TeraLCP constructs **LF, ψ, φ, φ⁻¹, LCP, PLCP and samples** from the
  RLBWT in **O(r) space, O(n) time, parallel**. Has a "Phi+samples" phase
  and a `--thr-pfp` mode that emits **pfp-thresholds-style 5-byte threshold
  files**. README reference: human472 (HPRC-class) at 172 GiB peak vs ~2 TB
  for prior tools.
- TeraIndex adds **LF + inverse-φ** to make a matching-statistics index;
  TeraMEM/TeraMS do MEMs and matching statistics.
- Caution: fresh code ("cite: TBA"); test data uses **ropebwt3 FMD
  (bidirectional)** while our streams are **BCR (forward-only)** —
  sentinel/convention handling must be gate-checked (rung 4a.1).

### sxgc χ layer (the product)
- One-pass suffixient scan over **threshold/LCP structures** (TeraLCP
  `--thr-pfp` 5-byte thresholds, byte-gated against the PFP-era references)
  → minimal-χ set → `.sA`.
- Yeast gate: χ = 85,404,240 (2.56% of n); `.sA` = 341 MB (10.2% of text).
- All results map to `sample#contig:offset` (`tools/mappos.py`).
- **Anchor→φ-locate hybrid** for all-occurrence enumeration (rung 4a.2
  decides: φ-based samples replace the O(n) sampler if they come at r-space
  cost).
- Capability contract: sA returns **all MEMs + one anchor each**;
  all-occurrence enumeration is the r-index/φ side.

### agc_text_oracle + ragc-ffi (`-o agc`)
- `libragc_ffi.so` cdylib over ragc-core pinned rev
  `40e5cad11cab7d4df07a72d6b16d68c2d60b0742`; C ABI:
  `sxgc_agc_open/len/range/close`; stored-cname addressing (CNV_NUM inside).
- 64 KiB window LRU, byte-budgeted; sidecar binary search for flat→contig mapping;
  `$` boundaries served by the oracle.
- The oracle serves sA MEM queries; queries 50/50 byte-verified (see 3.3).

### Legacy-reference constructors (yeast-gated reference + χ-legacy path)
- **pscan -S** (streamed PFP parse from stdin), **pfp_suffixient -A**
  (`.suff/.lcs/.mult` components), **rindex_build/rindex_query** (`.ri` v3),
  **rlbwt_sampler / `.ri4` v4 chain** — superseded for production by the
  substrate chain, retained as the byte-gated yeast references and the
  χ-legacy path (decision 13). Their contracts below remain binding for the
  reference path.
- pscan -S: `agc2flat ... --stdout | pscan <base> -S -w 10 -p 100 -t 1 -s`;
  `.parse/.dict/.occ/.0.sai/.0.last` at `<base>`.
- rindex_build: `.ri` format v3 (`"SXRI"` u32, version, n, sigma, R, C[256]
  u64, `run_char[R]` u8, `run_len[R]` u32, sdsl `int_vector` SA samples
  packed to `bits(n)` — 41 bits at HPRC scale). Query reads v1/v2/v3; v1
  (u32 SA) overflows past 4.29 Gbp — never at scale. `-n` convention: build
  takes n = stream symbols + 1 (sentinel-inclusive); query takes `-N` =
  stream symbols; mismatch shifts every position by 1 (caught by the
  planted-truth gate). Sentinel fix: `rank(c, i)` past the last c-run needs
  per-char sentinel totals — do not regress.
- rlbwt_sampler: parallel per-string LF walks over the v4 chain; S =
  fstart+L−j run-end samples; zero text access; 712/712 vs brute force;
  rindex_query v4: S−m locate (S increases per LF step — `sa_at` uses
  sample−steps).

### Pilot tooling (`tools/`, `bit6/`)
- `pilot_patterns.py`: weighted-random windows over the sidecar, extracted via
  ragc-ffi — zero materialization; writes patterns + planted-truth positions.
- `pilot_verify.py`: byte-verifies every reported occurrence through the AGC
  (precision) + planted-truth recall; single `PILOT VERDICT` line.
- `bit6/pilot_run.sh K SAMPLES`: staged runner — stream → build → patterns →
  query → verify, with df guards (400/300/250/200 GB floors) between stages.
- HPRC AGC sample names are assembly-style
  (e.g. `HG03927_pat_hprc_r2_v1.0.1`); contig names are PanSN
  (`sample#hap#accession`).

## 5. Status and plan — the rung-4a ladder (ACTIVE)

**Nothing scales past k=10 until rung 4a is green.**

### Proven (gate record in BIT_LADDER.md)
- **Bits 1–4**: Lean formalization (Def. 9, 511/511 exhaustive); scan-rs ==
  C++ byte-exact; AGC-native group construction; streamed `-A` components
  byte-identical.
- **Bit 5**: `-o agc` oracle — locate+MEMs 500/500 byte-verified, zero oracle disk.
- **Bit 6**: whole-collection yeast end-to-end, zero materialization:
  `.sA` = 341 MB, 50/50 verified; reader A/B (ragc stays); query cost structure
  measured; three critical build bugs found via small-scale discipline.
- **Rungs 1–3** (legacy PFP route): r-index from the same streamed PFP
  (5991/5991, zero text access); layout v3 packed SA; out-of-core
  `disk_vector` machinery — now the byte-gated reference route.

### Rung 4a — yeast-scale validation of the substrate (current)
1. **4a.1 TeraLCP yeast gate**: validate LCP/PLCP against the in-house
   PFP-era byte-gated references. Includes the FMD-vs-BCR
   sentinel/convention check (TeraLCP's test data is ropebwt3 FMD; ours is
   BCR forward-only).
2. **4a.2 φ+samples validation**: if SA samples come at r-space cost, retire
   the O(n) sampler (`rlbwt_sampler`); locate goes φ-based (TeraIndex's
   LF + inverse-φ matching-statistics index).
3. **4a.3 thresholds → χ gate**: TeraLCP `--thr-pfp` thresholds → one-pass
   sA builder → **χ gate = 85,404,240 at yeast**.

### Then (each rung gates the next)
4. **k=10 rework validation** of the substrate chain (grlBWT measured
   there: R = 1,859,825,801, 43.5 min, 5.95 GB).
5. **k=50** pilot.
6. **Full-466 HPRC v2** run — includes the grlBWT seekable-input decision
   (1.4 TB revlines temp on nvme: policy decision or shard+merge; see risks).
7. **HPRC v3** — the objective scale.

### Open (alongside, unchanged)
- sA anchor → φ/r-index toehold hybrid (`r-index-toehold/`); GAF emitter
  (`tools/mappos.py` namespace mapping exists).
- Lean `sorry` discharge (covering/minimality via Lemma 34); χ_tag research
  (`RESEARCH.md`); tag arrays over the .gbz (`tag-array/`).

## 6. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| **grlBWT needs a seekable input file** — at 466 that is a **1.4 TB revlines temp on nvme** | high, policy | policy decision or shard+merge; decide before full-466 |
| **TeraTools is fresh code ("cite: TBA")** | high | everything must pass sxgc's differential gates — byte-gated yeast references exist (rung 4a.1/4a.3) |
| **TeraLCP sentinel/convention mismatch**: its test data uses ropebwt3 FMD (bidirectional); ours is BCR (forward-only) | high | gate-check sentinel/convention handling in 4a.1 before any scale-up |
| gsacak/PFP int32 cap crossed at k=10 (FOUND, fixed — legacy route) | was blocker | dict > 2^32 chars SIGSEGVs the int32 build; PFP tools link **gsacak64** (`-DM64` propagates); re-gated byte-identical on yeast. Analogue of the lz77/divsufsort caps. |
| pscan own int-widths at k=466 (occ = 14.2e9 > 2^32) — legacy route only | must audit before any legacy 466 run | k=10/k=50 safe (305M/1.5e9 < 2^32); audit before the full run |
| pscan -S ~74 h single-threaded wall @ 466 — legacy route only | certain cost, accepted | superseded by grlBWT (43.5 min at k=10) for production |
| Legacy-route scratch peak ~2.4 TB vs 3.6 TB free | medium | df guards in runner; prompt unlinks; production substrate avoids this shape |
| `rindex_query` full-enumeration speed | low | correctness first; φ-locate (4a.2) / toehold sampling next |
| ragc numeric-encoding regressions | low | forbidden-byte validation + CNV_NUM round-trip |
| Lean `sorry`s outstanding (covering/minimality) | low | discharge via Lemma 34 planned |

## 7. Decision log

1. **AGC-native zero-materialization streaming** over flat materialization
   (disk constraint + artifact discipline); `pscan -S` stdin is the only route
   (FIFO probe failed: size-seeking parsers).
2. **ragc (Rust FFI) over upstream C++ libagc** — 25× faster at random
   substring access (26 vs 1.2 MiB/s), and upstream segfaults on
   ragc-written archives. A/B measured on the official HPRC archive.
3. **r-index (whole-collection) as the target artifact**; banded sA remains
   the fallback/baseline structure. Target band **≤ ~23 GB accepted**;
   v3 layout projects ~15.5 GB.
4. **Out-of-core disk-backed arrays** — essential, not an optimization:
   dict structures alone exceed RAM at 466 (~2.4 TB).
5. **Packed SA samples (v3)** — u32 samples silently corrupt past 4.29 Gbp
   (same family as the Bit-6 `atoi` bug); widths follow `bits(n)`.
6. **Differential-gate convention**: cross-route set comparisons are never
  exact (tie-breaking); gates = same-route byte-identical match + χ equality
  + Lean covering + planted-truth/oracle byte-verification.
7. **`$` separators per contig** (no chimeric cross-boundary substrings);
   **sidecar written in the same pass** as the stream — single source of
   coordinate truth.
8. **`-n`/`-N` conventions documented** (build: symbols+1 sentinel-inclusive;
   query: symbols) — mismatches shift positions by exactly 1 and are caught
   by planted truths.
9. **Yeast-first development; human scale touched only via staged pilots**
   (k=10 → k=50 → 466), each with oracle byte-verification gates.
10. **PFP tools build with M64 (gsacak64)** — the int32 build is capped at a
    2^32-char dictionary (k=10 HPRC dict = 5.68 G chars SIGSEGVs); 64-bit
    widths are the default for all future runs.
11. **TeraTools adopted as the RLBWT→(LCP, φ, samples, thresholds)
    substrate** (MIT); **grlBWT as the BCR-BWT constructor** (GPL-3,
    external tool).
12. **Goal restated**: suffixient array at HPRC v3 scale; v2 (466 haps) is
    the on-disk development vehicle.
13. **PFP r-index construction retired from production** (147 GB / >3 h vs
    grlBWT 5.95 GB / 43.5 min at k=10); PFP machinery remains the
    yeast-gated reference + χ-legacy path.
