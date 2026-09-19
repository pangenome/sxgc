# sxgc — Suffixient/Run-indexed Genome Collections

*Indexes over AGC-compressed genome collections, constructed by streaming
directly from the archive with zero materialization, queried in the
`sample#contig:offset` namespace. All numbers measured on the target
workstation (256 cores, 1 TB RAM, 14 TB NVMe) unless attributed externally.
Living status document — the gate-level record is `BIT_LADDER.md`; this file
describes the architecture and the plan.*

---

## 1. Mission

Build and query text indexes over **AGC-compressed genome collections** —
working **directly from the archive**, never materializing the flat text —
with all results mapped into the **`sample#contig:offset`** namespace.
Scaling: 235 yeast strains (validated end-to-end) → HPRC v2 (466 haplotypes,
1.4 Tbp, one 3.3 GB AGC) → ~10,000 haplotypes (cluster port).

**Two query capabilities, two layers:**

| Layer | Answers | Text access | Artifact size (HPRC v2 proj.) |
|---|---|---|---|
| **r-index** (`.ri`, rungs 1–3) | **all occurrences**, exact | **zero** — pure index arithmetic (backward search + LF-walk) | **~15.5 GB** (6.125 B/run × 2.53 B runs) |
| **sA + AGC oracle** (Bits 4–6) | **MEMs + one anchor each** | probes the text via ragc FFI, window-cached, served from the 3.3 GB archive | sA ≈ 10% of text; oracle = the archive itself |

The sA layer's binary search intrinsically touches the text (~37 scattered
AGC window-fetches per cold pattern at collection scale, ~121 ms); the
r-index's all-occurrence enumeration needs none. The eventual hybrid (sA finds
MEMs fast, feeds an anchor to the r-index as toehold) composes both.

## 2. Architecture

The sequence lives in exactly two forms: **compressed inside the AGC**, or
**transient in a pipe**. Everything else is derived structure.

```
AGC archive (3.3 GB @ 466 haps)          ── only source of sequence
 │  ragc (Rust): segment-granular random access (~26 MiB/s; 2.4 ms/64KiB)
 ▼
agc2flat --samples K.txt --reverse --stdout   ── streaming adapter
 │  byte-exact mirror of the forward flat text ('$'+rev(contig), last first),
 │  bounded RAM, no files; sidecar (.names.tsv, forward offsets) same-pass
 ▼  (64 KB pipe; pscan paces at ~5.25 MB/s/core)
pscan -S                                 ── prefix-free parsing
 │  single sequential pass → .parse/.dict/.occ on disk
 │  (k=10: 5.7 GB dict; k=466: ~99 GB projected)
 ▼
rindex_build (r-index)  or  pfp_suffixient -A (sA components)
 │  pfp_iterator computes the BWT in suffix order ON THE FLY from the PFP —
 │  BWT/SA never materialize. Out-of-core (rung 3): d/saD/lcpD/isaD/p/saP/
 │  isaP/ilist/pos_T/s_lcp_T are mmap-backed scratch (`disk_vector`),
 │  unlinked when dead; RAM keeps succinct structures + sacak workspaces.
 ▼
hK.ri (format v3)   /   .suff/.lcs/.mult → one-pass-build-index → .sA
    run-length BWT + SA samples packed to bits(n)
```

Query side:
- `rindex_query` — backward search + LF-walk over `.ri`; zero text access;
  reports forward-flat positions (sidecar maps them to `sample#contig:offset`).
- `locate`/`mems -o agc` — sA binary search probing text through
  `agc_text_oracle` (dlopen of `libragc_ffi.so`, sidecar flat-offset mapping,
  64 KiB window LRU, 256 MiB budget).

### Why this shape (constraints → consequences)

| Constraint | Consequence |
|---|---|
| AGC as only source; near-zero temp disk | streaming build (`pscan -S` stdin; the FIFO probe failed: `mt_process_file` seeks by file size) |
| 1.4 Tbp text vs 1 TB RAM | PFP instead of a real SA; **out-of-core disk_vector** for all PFP arrays (dict alone ~99 GB → 2.4 TB of arrays at k=466) |
| ≤ ~23 GB artifact (accepted target) | r-index over full-text structures; runs are sublinear (2.53 B at 466 haps vs n = 1.4 T) |
| reproducible subset science | `agc2flat --samples` (archive order preserved, loud unknown-name abort) + staged pilots |

## 3. Measured constants

### 3.1 Construction (yeast235: 3.34 Gbp, unless noted)

| Stage | Measured |
|---|---|
| χ (whole collection) | 85,404,240 (2.56% of n); `.sA` = 341 MB (10.2%) |
| BWT runs | 100,904,881 (3.0% of n — diverse-yeast pessimistic; HPRC v2 = 0.18%) |
| r-index build (out-of-core) | wall 639 s; anonymous peak **2.56 GB** (10.5 GB before rung 3) |
| pscan -S χ scan rate | **5.25 MB/s per core** — the bottleneck constant |
| ragc random access | 2.4 ms per 64 KiB fetch (26 MiB/s); **~25× faster than upstream C++ libagc**, which also segfaults on ragc-written archives |

### 3.2 Queries (yeast235 whole-collection)

| | |
|---|---|
| sA `locate -o agc` | 50/50 byte-verified; 121 ms/pattern scattered-cold (427 ms before window cache); warm class 10–35 ms |
| r-index full enumeration | 5991/5991 occurrences byte-verified, 50/50 vs brute force; 6m20 s for 50 patterns / 5,991 occs — correctness first, query-side optimization is open |

### 3.3 Human dict scaling (measured on real HPRC subsets, w=10 p=100)

| k (haplotypes) | n | \|D\| | \|D\|/n | pscan peak RSS |
|---|---|---|---|---|
| 3 | 9.03 Gbp | 4.24 GB | 47% | 7.85 GB |
| 10 | 30.15 Gbp | 5.68 GB | 18.8% | 10.0 GB |
| 466 (linear extrapolation) | 1.4 Tbp | **~99 GB** | ~7% | ~200 GB |

Novel dictionary growth is only **~205 MB/sample** (3→10) and sublinear;
occ/n steady at 1.01% (avg phrase ~99 bp).

### 3.4 k=466 projections (out-of-core design)

| Resource | Projection |
|---|---|
| Anonymous RAM (workspaces + succinct + run arrays) | **~350–450 GB** |
| Scratch on work mount | peak ~2.4 TB → ~1.9 TB after load-time unlinks |
| pscan -S streaming pass | **~74 h single-threaded** (the zero-materialization price) |
| `.ri` artifact | **~15.5 GB** (v3 layout) |
| Query-side RAM | ~60–70 GB (per-run csum/cruns materialization) — fits, optimization open |

### 3.5 External reference points (WABI 2025, Eskandar/Paten/Sirén)

| | HPRC v1.1 | HPRC v2.0 |
|---|---|---|
| Sequence | 257 Gbp | 1,317 Gbp |
| BWT runs | 3.89 B | 2.53 B |
| Whole-genome r-index build | 5.3 h | 19 h |
| FMD bidirectional | — | 133 h; peak 1.04 TiB |

## 4. Component contracts

### agc2flat (Rust, ragc-core)
- Modes: whole-collection; `--group <contig>` (+`--band S:E`); `--groups`
  (metadata); `--reverse --stdout` (the build stream, **validated byte-exact
  mirror**); `--samples <file>` (restrict to listed AGC sample names —
  one per line, leading-`#` comments; archive order preserved; unknown names
  abort; applies to all modes).
- Sidecar invariant: written in the same pass as the stream (metadata lengths
  can disagree with decompressed bytes — sidecar is ground truth);
  `cname \t fstart \t len` in forward flat coordinates.
- `CNV_NUM` conversion post-numeric-decode; 0x00–0x02 rejected.

### pscan -S (C++, upstream PFP toolchain, fork)
- Streamed single-thread parse from stdin: `agc2flat ... --stdout | pscan <base> -S -w 10 -p 100 -t 1 -s`.
- Produces `.parse/.dict/.occ/.0.sai/.0.last` at `<base>` on the work mount.

### rindex_build / rindex_query (C++ fork, `sA/suff-set-src/`)
- `.ri` format v3: `"SXRI"` u32, version, n, sigma, R, C[256] u64,
  `run_char[R]` u8, `run_len[R]` u32, then sdsl `int_vector` SA samples
  packed to `bits(n)` (41 bits at HPRC scale). Query reads v1/v2/v3.
  v1 (u32 SA) **overflows past 4.29 Gbp** — never use at scale.
- `-n` convention: build takes n = stream symbols + 1 (sentinel-inclusive);
  query takes `-N` = stream symbols. Mismatching them shifts every reported
  position by 1 (caught by the planted-truth gate).
- Known sentinel fix: `rank(c, i)` past the last c-run reads `csum[c]` out of
  bounds without per-char sentinel totals — do not regress.

### disk_vector (`sA/include/pfp_iterator/disk_vector.hpp`, rung 3)
- mmap-backed file vector; **stable element addresses** (the pfp priority
  queue holds raw pointers into `ilist`); scratch = unlinked on close;
  scratch names `<base>.dv.<what>.<pid>`.
- Big arrays out-of-core; RAM keeps only: `b_d`, `ilist_s`, both rmq's,
  sacak/gsacak workspaces, run arrays. Anonymous peak at yeast: 2.56 GB.

### agc_text_oracle + ragc-ffi (`-o agc`, Bit 5)
- `libragc_ffi.so` cdylib over ragc-core pinned rev
  `40e5cad11cab7d4df07a72d6b16d68c2d60b0742`; C ABI:
  `sxgc_agc_open/len/range/close`; stored-cname addressing (CNV_NUM inside).
- 64 KiB window LRU, byte-budgeted; sidecar binary search for flat→contig mapping;
  `$` boundaries served by the oracle.

### sA route (Bits 4–6)
- `pfp_suffixient -A -o <base> -w 10 -n <symbols+1>` → `.suff/.lcs/.mult`
  (5-byte-truncated LE records), byte-identical gates vs one-pass references.
- `one-pass-build-index -t sA` consumes the components (+ `-o agc` oracle);
  not PFP-dependent — unaffected by rung 3.
- Sorted-extract build discipline (positions extracted in sorted order, one
  sequential AGC sweep): build 5+ h → 9 min on yeast whole-collection.

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

## 5. Status and plan

### Proven (gate record in BIT_LADDER.md)
- **Bits 1–4**: Lean formalization (Def. 9, 511/511 exhaustive); scan-rs ==
  C++ byte-exact; AGC-native group construction; streamed `-A` components
  byte-identical.
- **Bit 5**: `-o agc` oracle — locate+MEMs 500/500 byte-verified, zero oracle disk.
- **Bit 6**: whole-collection yeast end-to-end, zero materialization:
  `.sA` = 341 MB, 50/50 verified; reader A/B (ragc stays); query cost structure
  measured; three critical build bugs found via small-scale discipline
  (u32 `atoi` overflow, N-from-absent-file, cache thrash).
- **Rungs 1–3**: r-index from the same streamed PFP (5991/5991, zero text
  access); layout v3 packed SA (u32 overflow blocker found; ~15.5 GB @ 466);
  out-of-core `disk_vector` machinery (byte-identical gates ×2; anonymous
  10.5 → 2.56 GB).

### In flight
- **HPRC pilots**: k=10 (the smoke set — cross-checks the 5.68 GB measured dict)
  streaming→build→oracle gate; then k=50 (deterministic stride selection,
  `bit6-k50.samples.txt`). Decide full-466 from measured R, build RAM, wall.

### Next
1. k=50 pilot; read the R(k) curve against the 2.53 B WABI number.
2. Full-466 run (single campaign): `--samples` all 466 → pscan -S (~74 h) →
   rindex_build (out-of-core, ~2.4 TB scratch peak, df-guarded) → query gate.
3. Query-side: toehold sampling for faster locate; EF `run_len` headroom
   (~2.6 B/run measured, H = 4.96 bits) if deep compression is ever wanted;
   csum materialization (~60–70 GB) → rank structures if needed.
4. sA anchor → r-index toehold hybrid (`r-index-toehold/`); GAF emitter
   (`tools/mappos.py` namespace mapping exists).
5. Lean `sorry` discharge (covering/minimality via Lemma 34); χ_tag research
   (`RESEARCH.md`); tag arrays over the .gbz (Phase 6, unchanged).

### Beyond
- ~10,000 haplotypes / 31 Tbp: per-band scan parallelism (runs are
  independent; merge by character) or cluster port; the out-of-core machinery
  is what makes a single-node 466 run possible and is the port unit.

## 6. Risk register

| Risk | Severity | Mitigation |
|---|---|---|
| **gsacak/PFP int32 cap crossed at k=10 (FOUND, fixed)** | was blocker | dict > 2^32 chars SIGSEGVs the int32 build; PFP tools now link **gsacak64** (`-DM64` propagates); re-gated byte-identical on yeast. Analogue of the lz77/divsufsort caps. |
| pscan own int-widths at k=466 (occ = 14.2e9 > 2^32) | **must audit before 466** | k=10/k=50 safe (305M/1.5e9 < 2^32); audit pscan parse/SA internals before the full run |
| 74 h single-threaded pscan wall @ 466 | certain cost, accepted | one long run; per-run-parallel scan is the known escape hatch |
| Scratch peak ~2.4 TB vs 3.6 TB free | medium | df guards in runner; prompt unlinks (isaD/saP/isaP/p ≈ 1 TB freed after load) |
| First out-of-core run at 10× yeast scale | medium | pilots k=10/k=50 before 466; byte-identical gates already proven |
| `rindex_query` full-enumeration speed | low | correctness first; toehold sampling next |
| gsacak/sacak workspace growth at \|D\|≈99 GB | medium | measured ~2×\|D\| at k=3/10 → ~200 GB projected; fits |
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
