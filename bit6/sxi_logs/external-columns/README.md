# THE EXTERNAL RUN-COLUMNS: the finish's run-scale columns leave RAM

Status: IN PROGRESS (this file is the live journal of the external-columns
lane; gates and the honest table are appended as they complete).

## What holds what (the exact per-phase column inventory, profiled at fragment
scale R = 397,723,010 / padded r = 397,723,016)

The finish chain is `rpfbwt_endpoints` (seam repair) -> `slim_dump` (per-run
aggregate columns) -> `xsa chi-rspace --stream-agg` (the sweep). Baseline =
the resident implementation at HEAD (fe5a197), re-run byte-identical to the
banked artifacts (`/tmp/extcols/frag-base`, `baseline_profile.sh`):

| Phase | Resident run-scale columns (bytes) | Peak RSS | Wall |
| --- | --- | ---: | ---: |
| endpoints: certify + repair structures | raw 32r=12.7 GB, starts 8r=3.2 GB, bychar 16r=6.4 GB, phi 16r=6.4 GB, headSa 8r=3.2 GB | (growing) | 8:01.55 total |
| endpoints: parse-free lf-base | +lfBase 8r=3.2 GB | 33.30 GB peak here | 2.5 s |
| endpoints: seeds sort | +seeds 16r=6.4 GB | 33.30 GB | 44.0 s |
| endpoints: walk + fingerprints | +checkpoints 0.39 GB | 33.30 GB | 71.9 s |
| endpoints: class LCE + 4 emit passes | (freed before) | 33.30 GB | ~270 s |
| slim: ri4 load | a 1r=0.40 GB, l 4r=1.59 GB, starts 8r=3.18 GB, samples 3.875r=1.54 GB | 6.56 GB | 3.9 s |
| slim: head_sa (mmap) + lf-base + seeds | head 8r=3.18 GB, lfBase 8r=3.18 GB, seeds 16r=6.36 GB | **19.38 GB** | 40.7 s |
| slim: LfIndex full build (dead weight in parse-free+head-sa mode) | charRuns 8r + charSum 8r = 6.37 GB | 19.38 GB | 10.3 s |
| slim: walk + fingerprints | checkpoints 0.39 GB | 19.38 GB | 73.2 s |
| slim: query phase (resolve+LCE+write) | (all of the above stay live) | 19.38 GB | 1527.9 s |
| sweep --stream-agg | none (already external) | 6 MB | 1:06.98 |

Resident totals at fragment: **endpoints 33.30 GB / slim 19.38 GB / sweep 6 MB**;
walls **8:01.6 + 27:25.9 + 1:07.0 = 36:34.5**. All byte-identical to banked.

## The externalization

All run-scale columns now live only in files (`bit6/ext_columns.hpp`):

* **ExtRuns**, the shared two-pass external form: one sequential pass over the
  run sources derives a 24 B/run RECORD column (starts, lfBase, char) plus
  sorted SEED columns (head-SA position, run id — and, when the seam repair
  needs it, the tail-sorted inverse for phi^{-1}) via external merge sort
  (128 MiB sort buffers, chunk files unlinked while open, multi-pass k-way
  heap merge). A 50 MB RAM coarse index (one starts value per 64 runs) plus one
  bounded 1.5 KB block pread answers run_of().
* **The query phases sweep run-range bands** (65536 runs), bulk-loading each
  column slice for the active band (a/l/head/samples/head-sa) — the
  stream-agg sweep pattern applied to the dump and the repair.
* **Checkpoints are external**: the tau-spaced suffix hashes live in a PFCK
  sidecar (header + count u64; format in ext_columns.hpp) read through a tiny
  per-thread cache. At pile the checkpoint column is r bytes = 480 GB — never
  resident.
* **The seam repair**: the raw/bychar/phi/starts/PaddedRuns vectors (≈49r)
  are gone; the repair builds the same ExtRuns over temp columns decoded from
  the RLE (two streaming decode passes), answers rank() from the per-char
  run-id list + record column, and answers phi/phi^{-1} from the sorted seed
  columns plus `.ssa`/`.ssa_t` preads. Its LCE instance is the same shared
  SlimLCEParseFree.
* **Fail-loud caps intact** (documented in ../slim-parse-free/README.md): the
  slim's `max(1e9, 128n)` and the repair's `max(1e8, 16n)` byte-denominated
  journals still refuse; the journal is now SHARDED per thread (see lever ii)
  with a bounded, documented overdraft before refusal of
  <= (threads-1)*(limit>>12) + one query's verification cap (~1.2% at gate
  scales).

## Time levers

* **(i) merge-time fingerprint streaming**: `cross_lcp_merge --emit-pf` makes
  the FINAL merge pass emit `<prefix>.pftext` (the normalized text M — byte
  identical to the walk's sidecar) and `<prefix>.pfck` (the same backward
  tau-spaced suffix-hash recurrence, over the merge's own in-memory M.M; tau
  = (8n+final_runs-1)/final_runs, identical to the walk's because final_runs
  is the padded run count). The slim consumes them with
  `--pf-text/--pf-checkpoints` (ADOPT mode: no walk, no seeds, no record
  column), and the endpoints repair accepts the same sidecars
  (`--pf-text/--pf-checkpoints` on rpfbwt_endpoints), skipping its padded
  walk. One emission serves the whole finish. The 1.31e12-step pile walk
  (20 h–3 days projected) is eliminated.
* **(ii) journal sharding**: the shared-budget journal charged 18.8e9 CAS
  retries on one word at fragment; all hot fingerprint counters and the
  journal are now per-thread sharded (one fetch_add per limit>>12 units).
  Workers flush at exit; reports are exact after flush.

## Gates

| Gate | Result | Evidence |
| --- | --- | --- |
| g0 differential (7 corpora: adapter parity vs legacy, walk parity vs source text, brute-oracle agg identity, adopt-mode agg identity, adopt-mode repair identity, fault injection, tau override, refuse-out-of-policy parity) | **PASS** (33 PASS lines) | `g0-external.log`, `g0_external_gate.py` |
| (a) fragment byte identity | pending | `gate_fragment_external.sh` |
| (b) memory force-test (RLIMIT_AS 8 GiB) | pending | `gate_fragment_external.sh` |
| (i) merge lever (four files byte-identical + adopt agg identical + pfck python recheck) | pending | `gate_merge_lever.sh` |
| (c) synthetic pile-shape (R=2e10, 50x fragment) | pending | `gate_synth_pile.sh` |
| (d) the honest table | pending | below |

## The honest table

Filled when the gates complete. (Fragment-scale numbers measured; pile
projections at R=4.8e11, n=1.31e12, 48 threads, this box.)

## Journal accounting bug (found by the first gate-A run, fixed)

The first external fragment run was byte-identical but its journal overcounted
(total_work 75.7e9 vs the exact verification_work + probe charges 23.97e9):
the band loop spawns fresh worker threads per band pass (~583K spawns at
fragment), which overflowed the automatic 512-slot shard table — all late
threads shared slot 511 and raced on a plain counter. Output correctness was
never affected (byte-identity held), but a racing journal can also LOSE
charges, weakening the fail-loud cap. Fix: work loops that spawn short-lived
workers register them explicitly (`slim_set_thread_shard(t)`, slots 0..47);
the automatic tid fallback remains only for the few long-lived threads (walk,
self-check, main). All gates below ran with the fixed accounting.

## CORRECTION (supervisor brief error, mid-run): the synthetic pile-shape gate is CANCELLED

The original brief asked for "synthetic columns at scaled R"; that was a
brief error — the real pile corpus sits at /mnt/nvme2n1/erikg/pile.txt
(1,307,910,802,540 bytes). Never fabricate pile-shaped data when real bytes
are at hand. All synthetic fixture scripts and data (gen_synth_pile.cpp,
gate_synth_pile.sh, /tmp/extcols/synthtest) were deleted; the measured
synthetic numbers were struck from the plan. The replacement gate (c) runs
the external finish chain on REAL pile prefixes: pile-1b (1 GB raw head -c
cut, provided by the supervisor, no boundary snap — if any document-alignment
check refuses on the raw cut, that refusal is reported as a finding, not
quietly repaired) and then a 10 GB raw cut under the same force-cap
discipline.

## Time lever (i) gate result (fragment, banked chunks re-merged with --emit-pf)

* The 16-chunk fragment merge with `--emit-pf` produces the four files
  **byte-identical to the banked artifacts** (rlebwt, meta, ssa, ssa_t all
  BYTE_IDENTICAL) plus `frag.pftext` (n bytes) and `frag.pfck`
  (tau=22, count=49,187,737 — exactly the walk's tau and count; final_runs is
  the padded run count the walk-mode formula uses).
* Emission cost as a side stream of the final pass: text 10.1 s +
  checkpoints 2.5 s (vs the external walk's 1076 s and the pile walk's
  projected 20 h–3 days).
* Independent python verification of the emitted .pfck: 2000 sampled
  window identities h(k) − BASE^w·h(k+w) == direct hash of the w bytes — PASS.
* ADOPT-mode slim (no walk, no seeds, no record column; consumes the
  sidecars): **agg byte-identical to banked**, wall 20:47.28 (vs resident
  27:25.9 and external walk-mode 26:46–36:47), peak RSS **30.2 MB**.
  The repair also accepts the same sidecars (adopt-mode repair byte-identical
  at g0 scale; exercised at the real-prefix gates below).

### Fragment gates (a)+(b) — verdict lines (raw driver logs in logs/)

Gate A (uncapped): ENDPOINTS/AGG/sA all BYTE_IDENTICAL to banked,
chi = 306164765 (N=1082130213, R=397723010). External walls: endpoints
21:18.82 peak RSS 389.8 MB; slim 36:46.65 peak 264.1 MB (first run: 19:23.45 /
26:45.72 / 748 s LCE wall — run-to-run variance from page-cache state; both
runs byte-identical); sweep 0:57.78 / 6 MB. Gate B, the same full chain under
RLIMIT_AS 8 GiB: all four artifacts byte-identical again, chi exact,
endpoints peak 393.9 MB / slim 264.1 MB / sweep 6 MB. The resident binary
under the same cap: `FATAL: std::bad_alloc`, rc=1 (it needs 33 GB here).
With the fixed sharding the external journal equals the resident journal
EXACTLY: queries=511,831,565 verified_symbols=13,698,612,984
hash_probes=735,171,861 verification_work=15,894,291,032
hash_probe_charges=2,940,687,951 total_work=18,834,978,983 — the same work,
byte for byte.

* Hygiene note: the /tmp working dirs of gates (a)/(b) — including their raw
  per-phase .log files — were deleted by my own disk cleanup before being
  copied here; every number above was journaled at the time it was measured,
  and the driver verdict logs are preserved in logs/.

## FINDING (real pile bytes, fail-loud x2): the raw prefix needs the production byte-remap

Running the chain AS-IS on the supervisor-cut raw prefix (no manipulation):

1. **Document boundary**: chunk_frontend processes all 16 chunks, then
   REFUSES LOUDLY: `CHUNK_FRONTEND_FATAL input must end at document
   boundary` (rc=1). The raw 1e9-byte cut ends mid-document ("...yste").
   Reported as-is; the chain gate uses the loudly-recorded snap to the next
   0x1e boundary (SNAP n=1000000665, +665 bytes, sha256
   c382daa53d0e66fdc921c32d356961b7e8189ceee0a435bb3b66e06b203ebb61; the
   first 1e9 bytes verified sha-identical to the raw cut).
2. **Padding-dollar collision**: even document-aligned, the RAW pile bytes
   fail the merge's final emit: `CROSS_LCP_FATAL padding count mismatch`
   (logs/real1b-merge-noremap-FAILED.log). The pile contains literal bytes
   below the padding range (1 GB cut: 0x01 x125, **0x02 x29**, 0x03 x3,
   0x04 x67, 0x07 x21; the 10 GB cut additionally has 0x03 x96494, 0x00 x18,
   0x02 x153). The cyclic padding dollar is 0x02; literal 0x02 bytes in the
   text break the distinct-rotations invariant and the merge refuses loudly
   instead of emitting a wrong structure. The banked fragment route passed
   the same bytes because it applied the PRODUCTION BYTE-REMAP
   (vendor/chunk-merge-v3/merged/frag.remap: 0x01->0xFF, 0x02->0xFD,
   0x03->0xFE, 0x04->0xFC, 0x1e fixed, bijective) BEFORE chunk_frontend.
   frag.remap is verified collision-free for both prefixes (no 0xFC-0xFF
   bytes occur in either cut, so nothing maps back onto the dollar). Both
   gates run chunk_frontend WITH this remap — recorded as a required
   production input for raw pile bytes, not a quiet repair.

## GATE (c) at 1 GB — REAL PILE BYTES: full pass, external == resident == adopt

Corpus: real pile prefix (first 1,000,000,665 bytes of the pile — raw 1 GiB cut
snap-extended 665 bytes to the next document boundary), n=1,000,000,665,
merged padded r=368,036,610, normalized R=368,036,609
(R/n=0.3680 — the pile family; fragment: 0.3676), chi = **283,296,933**
(chi/n=0.2833; fragment: 0.2831).

Three independent full finishes on the same merged files:

| chain | cap | wall (total) | peak RSS | verdict |
| --- | --- | ---: | ---: | --- |
| EXTERNAL, walk mode | RLIMIT_AS 8 GiB | 23:17.6 + 27:51.3 + 0:54.8 | **389 MB / 256 MB / 6 MB** | ri4/head_sa/agg/sA all byte-identical to resident; chi = 283296933 |
| RESIDENT (reference) | uncapped (needs it) | 7:45.5 + 25:11.4 + 0:52.5 | 30.78 GB / 17.89 GB / 4 MB | the reference; chi = 283296933 |
| ADOPT (merge sidecars, both walks skipped) | RLIMIT_AS 8 GiB | **5:45.9 + 19:54.2** | **386 MB / 30.2 MB** | ri4/head_sa/agg byte-identical to the walk-mode external outputs |

The external+sharded query phase beats the resident one on real bytes too
(slim LCE wall: external 784.1 s, resident 1278.4 s, adopt 1078.5 s —
run-to-run page-cache variance included, all at 48 threads). The resident
chain under the same 8 GiB cap cannot even start (the 33 GB fragment-scale
refusal is the same failure mode; the resident 1b endpoints needs 30.8 GB).
ADOPT — the pile-shaped path — is now FASTER than the resident finish at 1 GB
while holding under 0.4 GB.

## THE HONEST TABLE (gate d)

### Measured, real data (fragment = banked 1.08 GB pile prefix; 1b/10b = real
### pile prefixes through the whole chain). 48 threads for the finish,
### 24 for the 1b merge, 48 for the 10b merge.

| phase | resident-before: wall / peak RSS | external walk: wall / peak RSS | external ADOPT: wall / peak RSS |
| --- | --- | --- | --- |
| FRAGMENT endpoints (R=397.7M) | 8:01.6 / **33.30 GB** | 21:18.8 / **390 MB** | (n/a — banked inputs) |
| FRAGMENT slim | 27:25.9 / **19.38 GB** | 36:46.7 / **264 MB** | **20:47.3 / 30.2 MB** |
| FRAGMENT sweep | 1:07.0 / 6 MB | 0:57.8 / 6 MB | same |
| 1 GB endpoints (R=368.0M) | 7:45.5 / **30.78 GB** | 23:17.6 / **389 MB** | **5:45.9 / 386 MB** |
| 1 GB slim | 25:11.4 / **17.89 GB** | 27:51.3 / **256 MB** | **19:54.2 / 30.2 MB** |
| 1 GB sweep | 0:52.5 / 4 MB | 0:54.8 / 6 MB | same |
| 1 GB merge --emit-pf (sidecar cost) | — | 21:09.1 / 25.4 GB; sidecar emit +12.6 s | feeds ADOPT |
| 10 GB (R~3.68e9) | ~200-330 GB est. (not run; out of cap discipline) | (see 10b section) | (see 10b section) |

Byte-identity held at EVERY scale and mode measured: fragment external ==
banked == resident (gate a/b), 1 GB external == resident == adopt (gate c),
10 GB internal consistency + format gates (below). The 128n/16n fail-loud
journals charge EXACTLY the same work as the resident implementation
(queries/verified/probes/work all byte-equal at fragment).

### Disk working sets (transient temps unlinked on success)

At fragment (R=397.7M), the external finish adds these TRANSIENT on-disk
columns (all derived, all unlinked on success, peak concurrent):
records 24r = 9.5 GB, head-seeds 16r = 6.4 GB, tail-seeds 16r = 6.4 GB,
seed sort chunks 16r = 6.4 GB (unlinked as merged), padded a/len/charlist
13r = 5.2 GB, plus the sidecar n = 1.08 GB and PFCK 0.39 GB (kept only under
SLIM_PF_KEEP_TEXT or by the merge). Persistent artifacts are unchanged
(.ri4, .head_sa, .agg 32R, .sA) — the same formats the resident path writes.

### Pile projection (R = 4.8e11, n = 1.31e12; linear in the measured per-unit
### costs, same discipline as every projection in this project)

RAM (external + ADOPT, 48 threads): repair ~8r/64 = 60 GB coarse index +
class rows <= r/1000 (~8 GB) + band buffers ≈ **~70 GB**; slim ADOPT band
buffers + caches ≈ **< 1 GB**; sweep 6 MB. **Total ~70 GB — the 400 GB
budget is met with 5x headroom. The RAM blocker is gone.**

WALL (measured per-unit scaled): slim queries 1.29/run x 48 threads at the
measured 748-1278 s per 512M queries -> **10-21 days** for 6.2e11 queries
(the 128n-capped query wall is the dominant term; denser checkpoints /
warm text tier are the named follow-up levers); endpoints ADOPT ~ I/O
bound, ~40.8 r-bytes of transient temps through the disk at 1-3 GB/s plus
class LCE -> **~15-30 h**; sweep 32R = 15.4 TB at ~0.2-1 GB/s -> **~4-21 h**.
The ADOPT walk elimination removes the 1.31e12-step walk (was 20 h-3 days
even resident; external-walk ~1 us/step would be ~7.6 days).

DISK at pile — **the decisive row**:
  live artifacts (inputs 9.6 TB merge outputs + 1.79 TB sidecars + 8.7 TB
  ri4/head_sa + 15.4 TB agg + 3.0 TB sA) ≈ **38.5 TB minimum live**, plus the
  external repair's transient temps ≈ **40.8 TB peak** (records + two seed
  columns + sort chunks + padded temp columns) -> **~79 TB peak**.
  This box has ~2 TB free. **THE PILE DOES NOT RUN ON THIS BOX — not because
  of RAM (fixed: ~70 GB fits), but because the on-disk ARTIFACT WIDTHS are
  ~20x (live) to ~40x (peak) the available space.** The .agg alone (32R =
  15.4 TB) is 8x the free space. The next lane, if the pile must fit here,
  is artifact-format narrowing (sample columns at 41 instead of 64 bits,
  column widths, and a slim->sweep band pipeline that consumes the .agg as
  it is written), not more finish externalization.

## FINDING (10 GB, hard upstream ceiling): the chunk-merge route caps at n < 2^32

The 10 GB run (raw cut head -c 10,000,000,000, last byte 0x6e mid-document,
snap +17,258 bytes to the next 0x1e, sha256 of the snapped prefix
d440d9e1fe0aeb4c32ebeffb2df02dac009a0806510c2f864a04c2cd92d3864a; first
1e10 bytes sha-verified against the recorded raw-cut hash) chunked and merged
27 of its 31 tree pairs over 2:59 h — then failed LOUDLY at the L2->L3 load:

    CROSS_LCP_FATAL invalid SXCR header

Diagnosis (files inspected, not guessed): the L2-level intermediates are
valid SXCR files by SIZE and magic, but carry n = 2,500,077,385 — past the
format's hard load guard `require(n && n < (1u<<31) && runs)` (SXCR
positions are u32; the loader enforces 2^31). The L2 files were WRITTEN
without a guard and then refused on read-back. Consequence: in any binary
merge tree the penultimate intermediates are n/2, so the chunk-merge route
caps every tree-mergeable corpus at **n < 2^32 ~ 4.29 GB**. The pile corpus
(1.31 TB) exceeds this ceiling by **305x**. This is an UPSTREAM (merge lane)
blocker, not a finish one: the finish consumes the merged four files at any
scale, but the pile's four files can never be PRODUCED by this route without
64-bit positions in the SXCR intermediates (or a different merge topology).

To still measure the finish on real bytes past 1 GB, a ~4.29 GB prefix
(the maximum this route permits, +469,833-byte snap, n=4,290,469,833) is
running through the same chain under the same 8 GiB force-cap.

## THE 2^31 WIDENING (user directive: no caps) and its validation

The SXCR chunk intermediate's u32 positions + hard `n < 2^31` load guard are
WIDENED to 64-bit across the whole route (this was a bug on the road to the
pile, not a milestone):

* `bit6/chunk_frontend.cpp`: SXCR **v2** writer — 64-bit run lengths
  (25-byte run records), version bumped so v1 and v2 fail loud against each
  other in both directions (verified: a v1 reader refuses v2 chunks and the
  v2 reader refuses v1 chunks).
* `bit6/cross_lcp_merge.cpp`: v2 readers/writers everywhere; the `n<2^31`
  guard is gone (sanity bound 2^62); every position/order vector widened
  (`Side::pos`, `WM`, `lf`, `classes`, `anchors`, `repair` work arrays,
  `RankIndex::tab` counts, selftest oracles); the `.sxs` sidecar becomes
  **SXS2** with 64-bit positions (size 20+9n), old SXS1 refused loudly.
* The four final files (.rlebwt/.meta/.ssa/.ssa_t) and the PFCK sidecar are
  UNCHANGED (already 64-bit; the banked artifact contract holds).
* Intermediates are transient; no artifact regeneration needed.

Validation, real bytes, three independent proofs:
1. tiny differential (7 corpora): the whole widened route passes 33 checks —
   adapter parity vs the legacy parse binary, walk parity vs source text,
   brute-oracle agg identity, adopt-mode identity, fault injection
   (`g0-widened-chunk-route.log`).
2. **fragment scale**: the v2 route re-chunks pile-frag.txt (the real 1.08 GB
   pile prefix, production frag.remap) and re-merges it — all four merged
   files BYTE-IDENTICAL to the banked v1-produced artifacts, and the v2
   merge's --emit-pf sidecars feed the finish to a byte-identical banked
   .agg (`logs/v2frag.driver.log`). The widening is semantics-preserving at
   the artifact level.
3. the 10 GB gate below runs the widened route end-to-end on real bytes.

One more REAL-DATA finding while re-running at 4.29 GB: **frag.remap is
window-specific** — it lifts 0x01-0x04 but leaves 0x00/0x05 identity, and
the pile contains literal 0x00/0x05 beyond the first GB (4.29 GB window:
0x00 x1, 0x05 x13; 10 GB window: 0x00 x18, 0x05 x23). The chain fails loudly
at the endpoint adapter's alphabet contract (`FATAL: unsupported text
alphabet`, chars must be >= 6; 0x00 also breaks the padding-dollar-minimality
the $-order assumes). A complete 10-GB-window remap was built with the
ByteRemap discipline (every PRESENT byte -> >=6, 0x1e fixed, bijective;
never-occurring bytes take the sub-6 image slots, so no text byte can ever
map below 6) — `/tmp/extcols/remap-10b.bin`. The raw-bytes refusals are
recorded as findings; the remap is the production input preprocessing.

## MERGE PROFILE (directive: report before designing) — what composes the 332 GB

Measured per-phase peaks of the 10 GB final dollar pair (n=10,000,017,258,
48 threads; CROSS_PHASE lines in logs/real10b/merge.log):

| phase | peak RSS | added/released (arithmetic, bytes/position) |
| --- | ---: | --- |
| child load (L3 pair, n=5+5 GB) | 166.0 GB | posA+posB 16 B/pos (160 GB) + M2 build 2 B/pos (20 GB), child texts freed |
| hash-build | 263.7 GB | +H dense prefix hashes **16 B/pos (160 GB)** |
| anchor+repair | 271.0 GB | +bwt 1 B/pos + block/anchor arrays |
| cross-merge | **332.0 GB** | +WM merged order 8 B/pos (80 GB) |
| emit phases | 332.0 GB | (all resident through emit) |

Composition at the final pair: **children 16 B/pos + fingerprints 16 B/pos +
merged order 8 B/pos + M.M text 2 B/pos = 42 B/pos persistent**, plus
transients. Pile projection of the resident merge (n=1.31e12): H 21 TB,
children 21 TB, WM 10.5 TB, M2 2.6 TB — **tens of TB: the third pile
blocker** (after the artifact-width disk blocker, before the finish, which is
now bounded). Diagnosis confirmed: dense per-position fingerprints +
fully-resident children, exactly as the directive suspected; tree topology
and chunking are fine (the pair tree is balanced and its intermediates are
already on disk).

Design (per the directive's menu, after profiling):
* children: streamed through banded windows over their on-disk SXCR/SXS2
  artifacts (they already live there);
* M.M and the merged order: streamed scratch files on /mnt/nvme2n1;
* fingerprints: k-spaced sparse hash file (density = speed knob only —
  merge correctness is verification-carried: every probe's proposed prefix
  and boundary is fully require()-checked today and stays so);
* bounded block-sort buffers; adaptive rank index sampling so the per-node
  index stays O(64 MB);
* target: a few GB resident per node at any n; achieved B/pos stated after
  the gate.
Scratch for this phase relocates to /mnt/nvme2n1/erikg/extcols-scratch
(root is done carrying builds).
