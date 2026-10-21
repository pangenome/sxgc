# PHRASE RADIX — RESULTS: the gates are green, the thesis does not hold

Branch `phrase-radix-2` (base `phrase-radix-recover @ 1914080`, itself
`pile-week-v1 @ aa1a95c`). Design: `tools/binbwt/PHRASE_RADIX_DESIGN.md`.
This document reports what ran, verbatim, including the arm that says the
mechanism as designed cannot fire on the corpora we have.

Short version, for the reader in a hurry:

* The **machinery is correct and proven**: the phrase-radix oracle family is
  green against a brute-force SA (672 arms, 13,468 skips taken, exact SA
  order), and the product path with the flag ON is **byte-identical to the
  banks at 100 MB and 1 GB** (chi 29716349 / 283296933).
* The flag is **OFF by default** and, when off, no line of the mechanism runs:
  the same bytes, the same counters, the same product.
* The **thesis fails as built**: at 100 MB, 1 GB and 10 GB the skip fires
  **zero times**. `PHRASE_WORK skips=0` at all three scales, while level passes,
  level elements, comparisons and symbols are **bit-identical** ON vs OFF. The
  arm costs the parse (27 s/GiB, +66% producer wall at 1 GB) and buys nothing.
* The reason is structural, not a tuning failure, and it is proved from the
  tree's own counters in §6: **a phrase skip needs unanimity** (every row of the
  node resolves to the same phrase), and **the byte radix only ever creates
  nodes whose rows disagree at their depth**. `no_progress_levels=0` at 100 MB,
  1 GB *and* 10 GB is the certificate of that.
* Where the cost actually is: **leaf LCE sorts**, 84% (1 GB) / 83% (10 GB) /
  92% (100 MB) of construction CPU. The byte-0x20 subtree alone is 17% of the
  10 GB build's CPU. That is the number a phrase dictionary should attack — but
  through a *per-pair LCE* hook, not a whole-node certificate.

---

## 1. What landed

| file | what |
|---|---|
| `bit6/phrase_radix.h` (505 lines) | `PhraseOpts`, `PhraseStats`, `PhraseDict` (winnowed parse -> block pool -> direct-mapped offset-entry table), the byte-verified `lookup8`/`verify`/`copyAt`, `phrase_journal_dict` |
| `bit6/binbwt_slice.h:1203` | `bin_phrase_skip`: the skip lemma as one function (uniform-entry test, then per-row BYTE verification) |
| `bit6/binbwt_slice.h:1343,1509` | the two probe sites (inline recursion and the dispatched `bin_task_run`) |
| `bit6/binbwt_slice.h:1636` | `bin_phrase_now = wopt.phrase` (null when the flag is absent: no line runs) |
| `bit6/binbwt_slice.h:3000,3130,3174` | `PHRASE_WORK` journal |
| `bit6/cross_lcp_merge.cpp:5494` | the producer's `--phrase-radix` + 13 knobs, DEFAULT OFF |
| `xsa/src/chain.rs` | the PRODUCT front end: `--phrase-radix` + knobs, validated with the producer's own constraints, refused on the legacy route, appended to the producer argv, journalled as `V2_PHRASE` |
| `xsa/src/plan.rs:597` | `XSA_PLAN_V2_PHRASE`: printed only when armed, and it PRICES the arm (parse seconds + the sample array's RAM) |
| `xsa/runtime/bit6/*`, `xsa/SOURCES.sha256.json` | the packaged mirror, refreshed (`tools/package_xsa_sources.py` now carries `phrase_radix.h`, without which the packaged build cannot compile) |
| `tools/binbwt/oracle.cpp` | the phrase oracle family (§4) |

### The flag surface

Producer (`bit6/cross_lcp_merge.cpp`), all under `if (has("--phrase-radix"))`:

```
--phrase-radix --phrase-min-len L --phrase-min-freq F --phrase-max-len M
--phrase-seed K --phrase-window W --phrase-block-max N --phrase-pool-max BYTES
--phrase-table-bits B --phrase-reps R --phrase-skip-min-rows ROWS
--phrase-skip-min-len BYTES --phrase-probe-depth BYTES --phrase-probe-stride BYTES
```

Product CLI (`xsa build`), same names, DEFAULT OFF:

```
$ xsa build ... --plan-only            # 0 XSA_PLAN_V2_PHRASE lines
$ xsa build ... --plan-only --phrase-radix
XSA_PLAN_V2_PHRASE armed=1 (--phrase-radix) default=OFF product_path=unchanged parse_s=3 ...
  parse_samples=8340042 peak_sample_ram_gb=0.1 ... dict_ram<=pool_max (default 16 MiB)
  verify=byte-compared (a hash collision can only LOSE a skip, never produce a wrong order)
  effect=construction work is unchanged except at nodes a phrase certifies
$ xsa build ... --plan-only --phrase-min-len 32
xsa: error: build: --phrase-min-len needs --phrase-radix (the dictionary is off by
  default and its knobs do nothing without it)
```

`--legacy-merge` refuses the flag (the merge route has no bin tree), a knob
given twice is refused, and every knob is checked against the producer's own
`require(...)` (min-len/seed/skip-min-len >= 16, max-len >= min-len, window >= 2,
table-bits in [8,26], reps >= 2) so the front door cannot accept a dictionary the
producer would refuse hours later.

---

## 2. THE THESIS (as designed)

> The bin machine turns the corpus's repetition into DEPTH (`depth_max=854` at
> 10 GB). Every level of that depth is a count pass plus a scatter pass over the
> node's rows, with one random byte read per row per level. A phrase parse
> answers the same question once, content-addressed: "the text at `x` is entry
> `(block, offset)` of length `s`" turns `s` levels into ONE level of width `s`.

The skip is licensed by the SKIP LEMMA: at a node certified at depth `d`, if
**every** row resolves to the same entry `E` and **every** row's text at
`tail+d` equals `E` for `E.len` bytes, then re-certifying the node at
`d+E.len` is exact. The lemma is sound (and the oracle proves the code
implements it — §4). Whether its hypothesis is ever *satisfied* is a fact about
the tree, measured in §6.

---

## 3. THE PRODUCT IS UNCHANGED (byte-identity gates)

### 100 MB bank gate — both arms, current build (`phr-build/g100-*.log`)

```
=== 100m off start 2026-10-21T05:16:41+00:00 loadavg=34.73 36.52 38.90
producer rc=0
finish rc=0
cmp=0 BANK-IDENTICAL (100m off)
cmp=0 PRODUCT-IDENTICAL to the OFF arm (100m off)
PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515 container=phr-g100-off/product.sxi2
PHRASE_WORK armed=0 ... skips=0 ... level_passes=157 level_elems=18...
=== 100m on start 2026-10-21T05:16:56+00:00 loadavg=37.07 36.95 39.00
producer rc=0
finish rc=0
cmp=0 BANK-IDENTICAL (100m on)
cmp=0 PRODUCT-IDENTICAL to the OFF arm (100m on)
PILE_FINISH_DONE chi=29716349 n=100000503 r=38647515 container=phr-g100-on/product.sxi2
PHRASE_WORK armed=1 ... probes=3 probe_rows=1881990 skips=0 ... level_passes=157 lev...
ALLDONE
```

bank = `extcols-scratch/xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`.

### 1 GB bank gate — both arms, full product, producer and finish concurrent

```
=== ARM off start 2026-10-21T05:18:07+00:00 loadavg=38.35 37.75 39.15
=== ARM off producer_rc=0 finish_rc=0 e2e_s=150.993136657 end 2026-10-21T05:20:38+00:00 loadavg=65.81 53.48 45.12
cmp=0 BANK-IDENTICAL (off)
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=phr-p1b-off/product.sxi2
=== ARM on start 2026-10-21T05:20:39+00:00 loadavg=65.81 53.48 45.12
=== ARM on producer_rc=0 finish_rc=0 e2e_s=169.054764924 end 2026-10-21T05:23:28+00:00 loadavg=68.24 62.48 50.04
cmp=0 BANK-IDENTICAL (on)
PILE_FINISH_DONE chi=283296933 n=1000000665 r=368036609 container=phr-p1b-on/product.sxi2
=== arm vs arm
cmp=0 ON == OFF (1 GB product)
```

bank = `final4b-gate1b/mergperf-1b-snap.sxi2`. `e2e` is producer and finish
concurrent (wall of the slower): 151.0 s OFF, 169.1 s ON — the +18 s is the
parse overlapping the fused finish. (Both runs shared the box with other lanes;
loadavg is printed per arm.)

The predecessor's row-stream gate also showed the producer's published rows
identical (`.rlebwt`, `.meta`, 11,234 band pieces) at 1 GB; the product gate
above supersedes it by comparing the container itself.

---

## 4. THE ORACLE FAMILY (brute-force SA, each new mechanism its own arm)

`c++ -O3 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle`,
then `CROSS_POOL_SLOTS=65536 ./oracle <scratch>` (21 min, 1.4 GB of log).

```
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
BINBWT_ORACLE_WINDOW_PASS cases=12864 fixtures=134 modes=cyclic,dollar threads=3 level_bytes=1,2 cap=1,7 credit=1,2,3 build_cap=0,5 disc=1 ram=1 file_reuse_runs=11248 window_instances=176784 window_files=88392 exact_brute_SA=1
BINBWT_ORACLE_REFUSAL_PASS refusals=5 modes=unlink,truncate,stale,nomagic,nodone control=exact_brute_SA
BINBWT_ORACLE_STAGE_PASS cases=4824 staged_arms=2412 fixtures=134 modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 stage_hits=42 stage_bytes=297 counters_identical=1 exact_brute_SA=1 both_paths=1
BINBWT_ORACLE_PERIODIC_PASS cases=4824 fixtures=134 modes=cyclic,dollar threads=1,3,8 target=1,4,24 depth_cap=0 (AUTO, no cap) level_bytes=1 cap=7,all periodic_leaves=54 periodic_rows=2304 periodic_period_max=2 over_target_leaves=0 exact_brute_SA=1 window_instances=24822
BINBWT_ORACLE_ADAPTIVE_PASS cases=4016 fixtures=134 modes=cyclic,dollar threads=1,8 target=1,2,3,17 depth_cap=256 level_bytes=1 cap=7,all depth_max=64 leaf_rows_max_le_target=1 over_target_leaves=0 exact_brute_SA=1 window_instances=21736
BINBWT_ORACLE_FREQ_PASS cases=6432 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all freq_sched=1 exact_brute_SA=1 multi_window_runs=2904 window_instances=33096
BINBWT_ORACLE_PHRASE_PASS cases=672 arms=672 fixtures=7 parse_determinism=7 dictionaries=7 nonempty=6 empty=1 offset_entries_checked=571 offset_entries_lost=4 blocks_addressable=34 blocks_unaddressable=0 probes=385312 skips=13468 skip_rows=81432 skip_bytes=3366000 verify_refusals=0 skip_max_len=64 exact_brute_SA=1 midphrase=1 offsets=1 boundary=1 wrap=1 edit=1 nested=1 norepeat=1 empty_dict_counter_identical=24 modes=cyclic,dollar threads=1,3 leaf=1,4,24 level_bytes=1,2 L=16,20,200,4000 F=2,1000000,2^30 skip_min_rows=1 window_instances=23424
```

The phrase family asserts, per arm: the bin order equals the brute-force SA
order (cyclic *and* dollar), the dictionary is **deterministic** (same corpus +
flags => same dictionary, 7/7), an empty dictionary leaves the counters
**identical** to the flag-off run (24 arms), entries are addressable at their
offsets (34 blocks, 0 unaddressable), mid-phrase suffixes, the wrap boundary,
an edited copy, nested repeats and a no-repeat corpus all order exactly, and
**13,468 skips are actually taken** (81,432 rows, 3,366,000 certified bytes, 0
byte-verify refusals, max skip 64 bytes) with the SA still exact.

That is the important contrast with §6: **at fixture scale, where duplicate
clusters are dense and `skip_min_rows=1`, the skip fires constantly and is
right.** On the corpora, it never fires at all.

Front-end gates: `cargo test` **27 passed; 0 failed** (4 new phrase tests +
1 plan test: default-off and carried, knob-without-flag refused, legacy route
refused, the producer's constraint mirror, and the plan line absent/priced).

---

## 5. THE A/B — producer wall, level passes, symbols, and the depth journal

Every arm below is CONSTRUCTION (the `BIN_WORK`/`PHRASE_WORK` lines are the
arbiter). Counters are exact; walls carry the box's loadavg next to them.

### 1 GB (`mergperf-1b-snap.txt`, producer only, `--bin-probe`, 64 threads)

Alternating arms, one script (`phr-build/ab1b.sh`), so all four share the
environment.

| arm | wall s | parse_s | level_passes | level_elems | comparisons | symbols | leaf_sorts | depth_max | probes | **skips** |
|---|---|---|---|---|---|---|---|---|---|---|
| OFF | **41.21** | – | 648 | 2,709,186,546 | 21,184,557,983 | 223,383,555,397 | 62,228 | 193 | 0 | 0 |
| ON, shipped defaults | **68.31** | 27.56 | 648 | 2,709,186,546 | 21,184,557,983 | 223,383,555,397 | 62,228 | 193 | 14 | **0** |
| ON, dictionary ceiling¹ | **70.18** | 29.16 | 648 | 2,709,186,546 | 21,184,557,983 | 223,383,555,397 | 62,228 | 193 | 14 | **0** |
| OFF (repeat) | **40.50** | – | 648 | 2,709,186,546 | 21,184,557,983 | 223,383,555,397 | 62,228 | 193 | 0 | 0 |

¹ `--phrase-min-len 32 --phrase-block-max 262144 --phrase-reps 2
--phrase-table-bits 22 --phrase-pool-max 67108864`: 86,321 blocks / 1.89M
entries / 7.77 MB of phrase text — **63x the addressable dictionary of the
defaults** (427 blocks / 9,610 entries / 75 KB) — and exactly the same result.

Cost of the arm, measured: **+27 s**, i.e. **+66% producer wall for 0% of the
work**. `build_s` (24.33 OFF vs 25.45 ON) is the same work: the whole delta is
`parse_s`.

### 10 GB (`pf-10gb.txt`, constructor only, 64 threads, `--bin-probe`)

| arm | WALL s | maxrss | level_passes | level_elems | comparisons | symbols | leaf_sorts | depth_max | probes | **skips** |
|---|---|---|---|---|---|---|---|---|---|---|
| OFF (paired with the ON arms below) | **647.43** | 42.46 GB | 6,347 | 41,021,242,629 | 214,876,750,387 | 4,982,269,168,684 | 537,319 | 854 | 0 | 0 |
| ON, shipped defaults | **811.23** | 42.53 GB | 6,347 | 41,021,242,629 | 214,876,750,387 | 4,982,269,168,684 | 537,319 | 854 | 82 | **0** |
| ON, ceiling dict (L=32, 262144 blocks, 2^22) | *~790 (03:59->04:12)²* | – | 6,347 | 41,021,242,629 | 214,876,750,387 | 4,982,269,168,684 | 537,319 | 854 | 82 | **0** |
| OFF, contemporaneous repeat after the oracle³ | **731.73** | 42.48 GB | 6,347 | 41,021,242,629 | 214,876,750,387 | 4,982,269,168,684 | 537,319 | 854 | 0 | 0 |

² The ceiling-dictionary arm ran later, on a busier box (`total_s=524.743`,
`build_s=380.344`). Its counters are identical to the rows above, which is the
honest comparison; its wall is not a pair with anything.

³ Same shape, quiet box (loadavg 38.8 at start): `build_s=584.955`,
`total_s=720.523`, `build_cpu_s=19302.066`, `leaf_s=15907.296`, and `BIN_HOT`
identical to the first OFF row (bin=32 rows=1659881407 cpu_s=3349.218). The two
OFF walls (647.43 / 731.73) differ by 13% on an unchanged binary, which is the
honest error bar on any wall in this table; the parse is the exact number
(261.31 s default, 272.59 s ceiling) and the counters are exact.

Probe refusal accounting at 10 GB, ceiling dictionary:
`probes=82 probe_rows=485900731 refused_no_entry=10 refused_not_uniform=72` —
**72 of 82 probes die because the node's rows do not all resolve to the same
phrase**, and the remaining 10 because the 8-byte key at that position is not in
the table at all. Neither is a bug; both are the hypothesis failing.

### THE MONSTER: the byte-0x20 subtree at 10 GB (`BIN_HOT`, hungriest window)

```
bin=32 rows=1659881407 top_bins=0 sub_max=0 span_s=179.560 cpu_s=3254.792
thread_max_s=97.360 leaf_sorts=132688 leaf_elems=1659869192
level_elems=13371811781 symbols=863823722853 comparisons=35563886232
```

Interpretation against the 10 GB totals (`build_cpu_s=18894.874`,
`symbols=4982269168684`): the 0x20 bin is **17.2% of the build's CPU**
(3254.8 / 18894.9) and **17.3% of the symbols** (863.8e9 / 4982.3e9), carrying
1.66e9 rows. It is the single most expensive structure in the corpus — and the
phrase arm does not touch it: its cost is 1,660 million rows of **leaf LCE
sorting** (`leaf_elems` = 1.66e9 of its rows), not level passes.

### Where construction CPU actually goes (the number the next attempt needs)

| scale | leaf_s (leaf-sort CPU) | build_cpu_s | leaf share |
|---|---|---|---|
| 100 MB | 65.8/66.6 s | 71.2 s | **93%** |
| 1 GB | 1232.0 s | 1467.9 s | **84%** |
| 10 GB | 15747.5 s | 18894.9 s | **83%** |

---

## 6. WHY THE SKIP NEVER FIRES (this is the finding, not a bug)

Two independent facts, both from the tree's own counters, and the second one is
a proof:

**(a) The trigger's hypothesis needs unanimity.** `bin_phrase_skip` may only
re-certify a node if *every* row of the node has the same entry and verifies it
byte-for-byte for `E.len >= 16` bytes. That requires all `m >= 4096` rows to
agree on the **16+ bytes at the node's own depth** — in particular on the byte at
`depth`.

**(b) No node in these corpora has that, at any scale.** `no_progress_levels=0`
at 100 MB, at 1 GB and at 10 GB. That counter increments exactly when a node's
rows all fall in one bucket, i.e. when they agree on the byte at their depth.
Zero means: **every node the byte radix ever splits has at least two distinct
bytes at its own depth.** All `m >= 4096` nodes are split nodes (leaves are
`m <= target`, and the probe sits past the leaf return, so leaves are never
probed). Hence for every probeable node the unanimity requirement is false
before the dictionary is even consulted. A better dictionary cannot fix a
trigger that is unsatisfiable by construction.

The empirical confirmation is the probe bookkeeping: `probes=3` (100 MB),
`14` (1 GB), `82` (10 GB), all refused, `skips=0`, and the counters identical to
the flag-off arm at every scale. The two refusal reasons are exactly the two
ways the hypothesis fails: no entry there (coverage: 75 KB of phrase text for a
1 GB corpus = 7.5e-5; 7.9 MB for 10 GB = 7.9e-4), and "not uniform".

**(c) The census that motivated the design does not survive the shipped
configuration.** The design quotes a 6.05M-row leaf "needing 854-879 bytes of
shared prefix before it can be divided". The current tree on the same 10 GB
corpus journals `depth_max=854` (reproduced) together with
`over_target_leaves=0 over_target_rows=0 structural_leaves=0
no_progress_levels=0`: there is **no node it cannot divide**, so a 6.05M-row
exact-duplicate cluster cannot be in the tree today — such a cluster would be
unsplittable and would have to appear in `over_target_leaves` or
`no_progress_levels`. The historical number was taken under a **depth cap**
(the note says 256), where the cap, not the byte radix, ended the node. The
right reading of `depth_max=854` is "two suffixes share 853 bytes", reached by
*pairwise* sharing: at each of those 853 levels the node still splits.

So the design's premise 2 — "a 6.05M-row node split 879 times, `O(m x 879)`" — is
not a structure this machine creates. What it creates is 6347 level passes over
41.0e9 elements (4.1 per byte, already near the floor set by the data) plus
537,319 leaf sorts over 10.0e9 elements, and the leaf sorts are where 83% of the
CPU goes.

---

## 7. WHAT THE NEXT ATTEMPT SHOULD DO (recommendation, not implemented)

The dictionary itself is a good object: content-addressed, byte-verified,
deterministic, RAM-small (7.9 MB of phrase text answered 10 GB of corpus with
1.19M entries), and it costs 27 s/GiB to build. What is wrong is the *hook*.

1. **Move the hook into the leaf comparator (per-pair LCE), not the node.**
   `bin_leaf_...` compares suffixes by raw LCE from the certified depth; the
   leaf share of CPU is 83-93%. A dictionary that says "these 8 bytes are entry
   (block, o) of length s" can advance a *pair's* LCE by `s` after one
   byte-verified `memcmp` of the block — this is the case the data actually
   presents (dense pairwise sharing), and it is where the 0x20 monster's 1.66e9
   leaf elements are.
2. **Keep the offset-entry table** (it is what makes a mid-phrase divergence
   point a hit) and keep the byte verification; both are already load-bearing in
   the oracle.
3. **Price it in the plan** (already done: `XSA_PLAN_V2_PHRASE`) and gate it on
   the same byte-identity banks.
4. Do not re-run the whole 12h charter to find this out: the leaf-LCE hook is one
   function (`bin_leaf`'s comparator) plus the existing dictionary.

---

## 8. Residual risks

1. **Corpus specificity.** All three corpora are merged-genome snaps
   (`mergperf-100m/1b-snap.txt`, `pf-10gb.txt`), not the webtext pile the design's
   "boilerplate epoch" story is about. Nothing here measures a web pile; the
   finding is "on these corpora, at three scales", and the mechanism's premise
   (whole-node unanimity) is corpus-independent only to the extent that the tree
   never has single-symbol levels — which is what was measured, three times.
2. **`no_progress_levels=0` also means the periodic tie-break never fires**
   (`periodic_leaves=0` at 10 GB), so the same census argument applies to it.
   Not this lane's subject, but the surface is shared.
3. **The 10 GB walls are not a clean pair**: the ON arms ran at loadavg ~87 while
   OFF ran at ~38, and two OFF runs of the same binary differ by 13%
   (647.43 / 731.73 s). The exact sentence is "parse_s = 261.31 / 272.59 from
   the journal; counters identical"; the *wall* deltas (647 / 731 vs 811) are
   indicative and must not be quoted as a speedup.
4. **`bin_phrase_now` is a file-static** set per `bin_order` call. The producer
   runs one per process, so this is sound today; two concurrent `bin_order`
   calls in one process would race. Not defended against, on purpose.
5. **The registry payload budget test is RED repo-wide and was before this
   branch** (`bit6/test_cargo_sources.py::test_registry_payload_budget`:
   main 10,251,367 B and base 10,251,383 B against a 9,990,000 B budget; this
   branch 10,269,584 B, i.e. +18,201 B for the phrase sources). The snapshot and
   checksum tests pass. Fixing the registry budget is a repo-wide decision, not
   this lane's.
6. **The packaged mirror is a copy.** `xsa/runtime/bit6/*` + `SOURCES.sha256.json`
   were refreshed with `tools/package_xsa_sources.py`; any later C++ edit must
   re-run it or `test_first_party_snapshots_match` fails.
7. `PHRASE_DICT`'s `coverage_estimate` is labelled an ESTIMATE (winnow undersampling);
   the exact coverage is `PHRASE_WORK skip_bytes`, which is 0 on all three corpora.

---

## 9. Branch and commits

Branch `phrase-radix-2`, from `phrase-radix-recover @ 1914080`:

| commit | what |
|---|---|
| `00adb8a` | the front-end flags (xsa CLI + plan), the packaged mirror, and the one replay artefact removed |
| `cab60d3` | the gates, the A/B, and the finding (this document) |
| `a104a1c` | the contemporaneous 10 GB OFF arm and its 13% wall error bar |

Inherited from `phrase-radix-recover`: `2b6654d` the design document, `12d925f`
the insurance WIP, `ad73a08` the parse + tree hook + oracle family, `90b3a2f`
the DEEP+LARGE trigger fix, `1914080` the supervisor's recovery of the
uncommitted tail.

Reconciliation of `1914080` against the dead charter's session journal: exactly
one replay artefact remained (the `--phrase-probe-depth`/`--phrase-probe-stride`
pair parsed twice in `cross_lcp_merge.cpp`), now removed; the recovered
`phrase_radix.h` is functional as committed (the PHRASE_DICT format has no
`-Wformat` warnings and every journaled field appears exactly once), and the
whole oracle family including the new phrase arms is green on the final tree, so
the remaining order-fragile journal mutations were already reflected in the
state committed at `1914080`.

---

## 10. Repro

```sh
# build
c++ -O3 -std=c++17 -pthread bit6/cross_lcp_merge.cpp -latomic -o binbwt
c++ -O3 -std=c++17 -pthread tools/binbwt/oracle.cpp -latomic -o oracle

# oracle family (21 min)
CROSS_POOL_SLOTS=65536 ./oracle <scratch>

# 100 MB bank gate, both arms (product)
# 1 GB bank gate, both arms (producer + fused finish concurrent)
# 1 GB construction A/B, alternating off/on/on-ceiling/off:
#   see /mnt/nvme2n1/erikg/phr-build/{gate100m,product1b,ab1b}.sh
```

Machine: 256 cores, 1 TB RAM, `/mnt/nvme2n1` scratch shared with other lanes
(loadavg is printed beside every wall above).
