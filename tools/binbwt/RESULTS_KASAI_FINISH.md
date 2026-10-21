# RESULTS — kasai-finish: the column lands, the finish's LCE organ dissolves, and the 1 GB wall does NOT move

Branch `kasai-finish`, base `pile-week-v1 @ 1d86e84`. Design + disc plan + gates:
`tools/binbwt/KASAI_FINISH_DESIGN.md`. Everything below was produced by the REAL
binaries of this tree (`cross_lcp_merge --binbwt` with `--sa-sample-k 1` →
`kasai_column` → `pile_finish`), against the BANKED containers.

**Load: this box carried foreign work throughout** (`impg genome-infer`
diarisation, ~40-60 cores busy; every wall below is labelled with the load
average at the arm's start). None of these walls is a quiet-box number.

**The headline, stated before the tables so the tables can only confirm or
refute it:** the column removes the finish's LCE work completely and exactly
(12,820,424,563 verified symbol reads → 320, 669,260,274 hash probes → 21, on a
container byte-identical to the bank), and it does **not** move the 1 GB finish
wall (116.2 s → 115.5 s), because at 96 threads that wall is the **serial fold**
(`feed` 43.3 s + `advance` 29.7 s + `readband` 11.9 s) plus the m1/m5 encode
(22.8 s) — the verification was already 96× parallel and 96% overlapped
(`exposed_lce = 3.676 s` of 116 s). What the column buys is the *work*: 6,585 s
of thread CPU per corpus gigabyte, and the exposure, which is measurable the
moment the pool is the binding term (5.1× at `PILE_FINISH_POOL=8`, §4.3).

## 1. Gates (all green, all verbatim)

### 1a. The tiny brute-force oracle (`kasai_column --selftest`)

```
KASAI_ORACLE_PASS corpora=14 arms=42 column_entries=10200 run_families=2181
escaped_entries=4410 invert_arms=direct,bucketed3,bucketed1 escape_cap=255
```

14 adversarial corpora (all-equal, periodic `abab`, periodic `aab`, separators
`0x1E`, `dollar`-cyclic, `n=1`, `n=2` equal and unequal, a full descent, a Fibonacci
word, `deep-repeats` (300-symbol `a` runs), random over 4 symbols, random bytes,
a 1 KB binary corpus) × **3 inversion arms** (direct, 3 buckets, 1 bucket) × the
whole pipeline: every `col[r]` against a brute-force rotation SA + brute cyclic
LCE, **and** every derived `topLcp`/`interior` for the corpus's own BWT-run
structure. The escape path is *required* to fire (`escaped_entries=4410`).

The negative arm must die, and does:

```
KASAI_ORACLE_NEGATIVE expecting the coverage check to fire
KASAI_COLUMN_FAIL SA stream is not a permutation: position 4 appears twice
exit=2
```

### 1b. The inversion is an exact permutation, twice over, byte-identically

100 MB fixture, one SA stream, two independent scatter arms:

```
col-direct.sxlc.isa  vs  col-buck.sxlc.isa   -> ISA_IDENTICAL_direct_vs_bucketed64
col-direct.sxlc      vs  col-buck.sxlc       -> COLUMN_IDENTICAL_direct_vs_bucketed64
```

The bucketed arm ran with `--ram-budget 67108864 --buckets 64` (64 position
buckets, 1.2 GB of dump written and consumed) — the bounded-RAM arm the pile
needs — and reproduced the direct arm's 4,000,020,044-byte ISA and 118,066,903-byte
column **byte for byte**. Per bucket the gate is a strict-coverage proof (a
bucket's entries must be exactly its own position range, each exactly once), so
a dropped or duplicated position is fatal at the bucket, not at the end.

### 1c. The audit arm: EVERY value the sweep consumes, asserted against the walk

`PILE_FINISH_KASAI_AUDIT=1` computes both the column answer and the
`SlimFingerprint` walk for every run's `topLcp` and `interior` and `pf_check`s
them equal — 473,695,414 queries at 1 GB, 49,845,539 at 100 MB:

```
100 MB  audit: RC=0 CMP=0 CHI=29716349 SLIM_VERIFIED=1176027302   (= old 1176027277 + 25 seam)
1 GB    audit: RC=0 CMP=0 CHI=283296933 SLIM_VERIFIED=12820424883 (= old 12820424563 + 320)
```

Zero mismatches. The only queries that are *not* column reads are the rank holes
(§3) and the seam classes, and those run the walk itself.

### 1d. The banks: byte-identical, ON and OFF, at both fixtures

| fixture | arm | chi | cmp vs bank | sha256 |
|---|---|---|---:|---|
| 100 MB | old (`PILE_FINISH_KASAI=0`) | 29,716,349 | **0** | `b4753899592c9525…b3a289ba5` |
| 100 MB | **kasai** | 29,716,349 | **0** | `b4753899592c9525…b3a289ba5` |
| 100 MB | audit | 29,716,349 | **0** | `b4753899592c9525…b3a289ba5` |
| 1 GB | old | 283,296,933 | **0** | `38f03eb43aaea599…fd9d23144f` |
| 1 GB | **kasai** | 283,296,933 | **0** | `38f03eb43aaea599…fd9d23144f` |
| 1 GB | audit | 283,296,933 | **0** | `38f03eb43aaea599…fd9d23144f` |

ON == OFF products identical at both scales (same sha), and both equal the
banked containers.

### 1e. The repo's existing oracle suites (the `NRun` rank field + the new
seam-splice rank stamping must not move a single emitted run)

```
CHUNKED_REPAIR_ORACLE_PASS streams=128 widths=3392 workers=4 both_coalescers=1
  class_flanks=1 padding_straddle=1 dollar=1 legal_00_05=1
SWEEP_STATE_CARRY_ORACLE_PASS streams=2048 splits=34785 NUL=1 byte05=1 singleton=1
  periodic_colors=1 empty=1 ordered_witnesses=1 final_state=1
SEAM_SPOOL_READER_ORACLE_PASS empty=1 adversarial_orders=2 padded_flanks=1 NUL=1
  byte05=1 malformed_refusals=3
BAND_PROGRESS_READER_ORACLE_PASS trivial_bases=1 band_only=1 deferred_class=1
  EOF_clamp=1 terminal=1 delayed_full_band_EOF=1 backward_terminal_refused=1
  malformed_refusals=12
```

(the chunked-repair oracle now also compares `NRun::rk` between the serial and
the chunked replay, i.e. the rank base is *proved* schedule-independent.)

### 1f. The column builder's counters (the ratchet gate, exact)

```
100 MB  kasai_comparisons=200005074  kasai_n_ratio=2.000  walk_s=3.042  invert_s=22.115
        column_bytes=118066903 escapes=1129147 max_lcp=34067 mean_lcp=39.936501
1 GB    kasai_comparisons=2000002443 kasai_n_ratio=2.000  walk_s=22.887 invert_s=242.544
        column_bytes=1214951321 escapes=13434413 max_lcp=97059 mean_lcp=52.288860
```

`kasai_comparisons = 2.000n` **exactly** at both scales (the gate is `<= 3n`; a
broken ratchet costs 53n, measured by `sa_anchor_verify --full`).

Two independent corroborations of the column against machinery that shares no
code with it: `max_lcp = 97,059` is *exactly* the `max_lcp` the banked
`sa_anchor_verify --full` measured on the 1 GB full SA, and `mean_lcp = 52.2889`
is that verifier's `mean 53.289` minus exactly 1.000 (their counter includes the
terminating mismatch comparison, mine does not).

## 2. The pipeline cost (what the column ADDS)

| stage | 100 MB | 1 GB | per-GB rate |
|---|---:|---:|---:|
| producer side-stream write (`8n`, sequential) | 0.535 s (800,004,072 B) | 5.485 s (8,000,005,368 B) | 5.5 s/GB @ 1.46 GB/s |
| producer wall, k=1 ON (whole producer) | 5.20 s | 43.31 s | — |
| inversion (`--invert direct`, ISA `4n` + bitmap) | 22.1-30.4 s | 242.5 s | 242 s/GB |
| Kasai walk (96 threads) | 3.0 s | 22.9 s | 23 s/GB |
| column bytes (`1 B` + escapes) | 118,066,903 = **1.18 B/rank** | 1,214,951,321 = **1.215 B/rank** | — |
| ISA (locate table, `4n`) | 400,002,044 | 4,000,002,692 | 4 GB/GB |

The inversion is the expensive stage, and it is *not* the Kasai walk: 242 s vs
23 s per GB. It is one random store per position into a 4 GB slot array
(`MADV_HUGEPAGE` already applied; the box was at load 40-60, so the number is a
loaded-box number).

## 3. What the run stream actually looks like (measured, and it is not what the charter assumed)

`PILE_FINISH_KASAI_CLOSURE` at 1 GB:

```
bands=5615 rank_holes=39 disorder=5 noncontiguous_runs=14 walk_fallbacks=39
max_rank_end=1000000663 n=1000000665
```

* **39 rank holes**: the run stream does not tile the rank space. The seam-class
  splice advances the transducer's `row` past the rows its reconstructed runs
  describe; the terminal row is also emitted LATE (at 100 MB rank 1186920
  arrives after rank 1194188).
* **14 non-contiguous runs**: a *single* run whose two endpoint rows skip a rank
  inside the run (e.g. a `len=2` run at ranks {4,576,075 , 4,576,077}).
* Therefore **a cumulative sum of the run lengths is not the rank**, and neither
  is the transducer's own row: the first implementation shifted every subsequent
  rank by one and the audit caught 159 consecutive wrong values inside one band.
  The rank comes from the **locate table** (`ISA[head]`, one random read per run,
  in the pool), and the 39 holes take the walk for that one query each: **39
  fallback walks in 473.7M queries**, journalled, not hidden.

## 4. THE A/B: 1 GB, 96 threads (`PILE_FINISH_POOL` default)

| | old (`KASAI=0`) | kasai | delta |
|---|---:|---:|---:|
| **finish wall** | **116.2 s** | **115.5 s** | **−0.7 s (−0.6%)** |
| sweep | 92.9 s | 96.2 s | +3.3 |
| ├ `readband` | 11.9 | 7.7 | −4.2 |
| ├ `feed` (serial fold) | 43.3 | 47.1 | +3.8 |
| ├ `advance`-witness (serial) | 29.7 | 34.5 | +4.8 |
| ├ `lce` span | 91.8 | 95.5 | +3.7 |
| └ `lce` **exposed** (waited) | **3.676 s** | **0.173 s** | **−3.5 s** |
| m5-ef | 6.4 | 6.6 | +0.2 |
| m1-runs | 16.3 | 17.2 | +0.9 |
| **queries** (fingerprint) | 473,695,414 | 48 | −473,695,366 |
| **verified symbols** | **12,820,424,563** | **320** | **−12,820,424,243** |
| **hash probes** (the locate ladder) | **669,260,274** | **21** | −669,260,253 |
| max RSS | 4.9 GB | 8.8 GB | +3.9 GB (the 4 GB locate table) |

Loads: old 34.9, kasai 48-61, audit 57.3 — the kasai arm ran under a heavier
box, which is worth more than the 3-4 s it "lost" on the serial stages.

**The reading that matters:** the verification's *work* is gone (12.8e9 → 320
symbol reads; 669M → 21 probes; 6,585 s of thread CPU → ~0) and the finish's
*wall* is unchanged, because the wall was never the verification's — it is the
serial fold (`feed`+`advance`+`readband` = 85-89 s) plus the m1/m5 encode
(22.8 s). The `lce` span (91.8 s) is not the verification's duration either: it
is the band-pipeline window the verification rides inside, and its **exposed**
part was already 3.7 s of 116 s.

### 4.1 The 100 MB A/B, 96 threads

| | old | kasai | audit |
|---|---:|---:|---:|
| finish wall | 12.09 s | 11.30 s | 11.19 s |
| queries | 49,845,539 | 10 | 49,845,549 |
| verified symbols | 1,176,027,277 | **25** | 1,176,027,302 |
| hash probes | 41,570,537 | **0** | 41,570,537 |

Both `cmp=0` against `xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`.

### 4.2 THE NUMBER: the finish's per-GB rate

| arm | 1 GB finish wall | per-GB rate | verification CPU released |
|---|---:|---:|---:|
| old | 116.2 s | **116 s/GB** | — |
| kasai | 115.5 s | **115.5 s/GB** | 6,585 s/GB (thread CPU) |

**Flat.** The column does not change the finish's per-GB rate at fixture scale;
it changes what the finish is made of. Any launch table that prices the pile's
finish term must therefore be *fold*-priced, not LCE-priced — and the sibling
documents' "9 h of a 43.3 h pile finish" for the LCE term is the CPU, not the
wall, unless the pool is starved (next section).

### 4.3 Where the column DOES pay: the pool as the binding term

`PILE_FINISH_POOL=8` (8 verification threads instead of 96), 100 MB fixture,
both arms on the identical piece set:

| arm (POOL=8) | finish wall | `lce` span | verified symbols | cmp |
|---|---:|---:|---:|---:|
| old | **53.54 s** | 50.8 s | 1,176,027,277 | 0 |
| kasai | **10.44 s** | 7.8 s | 25 | 0 |

**5.1× on the finish**, byte-identical product. That is the pile regime in
miniature: when the machine is oversubscribed (the pile runs the finish against
the producer and the rest of the pipeline), the verification becomes the wall and
the column removes it entirely.

## 5. What this says about the pile (projection, labelled as such)

`n = 1.31e12`, `w = 41` bits, `R = 4.82e11` runs (r/n = 0.368 from the fixture):

| term | before | after | source |
|---|---|---|---|
| verification CPU | 6585 s/GB × 1310 GB ≈ **8,600 core-hours** | ~0 | measured 1 GB, `lce` region cpu |
| LCE wall exposure | ≤ 3.7% of the finish when the pool is free; **the whole verification time when it is not** | ~0 | §4, §4.3 |
| finish wall | fold-set (projected 43.3 h) | fold-set | §4.2 |
| disc, transient | — | SA stream 10.5 TB + ISA 6.7 TB (packed) + column 1.31 TB | design §3 |
| added producer cost | — | 10.5 TB sequential write ≈ 2 h at the measured 1.46 GB/s | §2 |

The pile's own numbers for the inversion (242 s/GB at fixture scale with a
page-cache-resident 4 GB ISA) must NOT be extrapolated: at the pile the ISA is
6.7 TB and the scatter's random stores hit disc-backed pages. That is the same
class of risk as the Kasai walk's random neighbour reads
(`KASAI_FINISH_DESIGN.md` §5) and it is the first thing a pile-shaped arm must
measure.

## 6. What was NOT run, and why (honest scope)

**The 10 GB arm was scoped and not attempted.** The blocking term is not the
producer (43 s/GB measured here) but the Kasai walk's **random neighbour reads**:
at 10 GB the SA stream is 80 GB and the walk makes one random read into it per
text position (1e10 of them), which no page cache holds. At the 1 GB fixture the
same reads are cache-resident and the walk costs 22.9 s; that number is *not*
extrapolable, exactly as `KASAI_FINISH_DESIGN.md` §5 warns. Making stage 3
pile-shaped (rank-blocked / two-pass) is the next charter, not a knob.

The inversion has the same shape of risk at the pile (§5) and, unlike the walk,
was measured in its bounded-RAM form here (64 buckets, byte-identical output).

## 7. Residual risks

* **The rank holes are a real property of the finish's run stream** (39 holes,
  14 non-contiguous runs, 5 order inversions at 1 GB). Every one of them takes
  the walk, and every one of them is *counted*; a future change that multiplies
  them (a class-table change) would show up in `walk_fallbacks` and in the
  column's cost, not silently.
* **The locate table costs one random read per run** (`4n` bytes at fixture
  scale, `w/8·n` = 6.7 TB at the pile, unpacked in this implementation). At
  fixture scale it is page-cache resident and free; at the pile it is a random
  read over cold disc unless the runs are rank-blocked by the producer, which
  they nearly are (the stream is ascending apart from the 5 inversions) — but
  "nearly" is not a measurement.
* **The inversion is the expensive stage** (242 s/GB direct; the bucketed arm
  measured 19.3 s at 100 MB with `--buckets 64`, i.e. *faster* than the direct
  arm's 22-30 s because the scatter stays inside one bucket). The pile must use
  the bucketed arm, and its cost is 36n bytes of I/O (§2), not extrapolable from
  here.
* **The T3 claims checker is untouched** — deliberately, and it still runs as the
  certainty gate over the finished product. The claims *producer* tier
  (`PILE_FINISH_LCE_TRUST=1`) is refused in the Kasai arm: there are no band LCE
  queries left to claim about.
* **`max_rank_end` (1,000,000,663) < n (1,000,000,665)**: two rank rows are never
  reached by any run at 1 GB. This is the same hole phenomenon, and it is why the
  closure gate is `<= n` plus the hole census rather than a tiling assert.
