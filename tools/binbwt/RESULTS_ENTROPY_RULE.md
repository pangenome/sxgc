# ENTROPY RULE — RESULTS: the stopping rule is exact everywhere, cuts the leaf
# term 54–73% in work and 24–33% in CPU, and does NOT move the 1 GB producer
# wall; the degenerate-bucket exit it exists for never fires on the 1 GB bank
# (it fires on adversarial fixtures and on a synthetic monster, measured below)

Branch `entropy-rule`, base `pile-week-v1 @ 88a7cb0`. Design:
`tools/binbwt/ENTROPY_RULE_DESIGN.md` (θ derivation, cost model, policy). This
document reports what ran, verbatim, and where the built thing differs from the
design — the design says the results document carries the corrections.

**Load: this box carried foreign work throughout** (load average 20–135 depending
on the minute; every wall below is labelled with the load at the arm's start).
Only tightly interleaved pairs are compared as walls. The work counters
(`comparisons`, `symbols`, `level_elems`, `depth_max`) are exact and load-free; the
timings are not, and the report treats them as such.

## 0. The headline, stated before the tables can refute it

* **The rule is exact.** The armed product is **byte-identical to the banked
  container** at 100 MB and 1 GB (`chi = 29716349` / `283296933`, `sha256` == the
  bank, `cmp` == 0), and the oracle proves order identity against the brute SA
  and the target-size-only arm on 7,728 armed cases.
* **On the bank corpus the stopping rule does not save wall at 1 GB.** The work
  drops hard — `comparisons` 21.18e9 → 9.67e9 (−54.4%), `symbols` 223.4e9 →
  166.8e9 (−25.3%), leaf CPU 1613 → 440 CPU-s (−72.8%), total build CPU −27.5% —
  but `build_s` is a **wash within the ±10% noise of a loaded box** (off
  21.9/24.0 s vs armed 22.5/23.8 s), because the exits make multi-million-row
  leaves (5.92M rows) and the 96-thread schedule loses occupancy: producer
  efficiency `build_cpu_s/(96·build_s)` falls from 0.95 to 0.67–0.72.
* **At 100 MB and 32 threads the wall DOES improve**: 2.27 s → 1.72 s (−24%),
  CPU 68.2 → 45.6 (−33%), leaf CPU 62.6 → 18.6 (−70%). **At 96 threads it does
  not**: probe wall 1.250 → 1.625 s (+30%) with CPU −2% and leaf CPU −71%, and
  the product's producer-only sink path is 3.42 → 3.56 s (+4%) — the same
  occupancy cause as at 1 GB, so the wall win is a low-thread-count effect.
* **The degenerate (H=0) tree exit the charter exists for did not fire on the
  1 GB bank**: `entropy_noprog_exits = 0` of 370 evals. The 1 GB `0x20` bin's
  cost is **leaf sorting**, not levels (`leaf_elems` = 167.2M of its 167.2M rows),
  so the lever that touches it is the **in-leaf arm** (−44% of the bin's
  comparisons, −26…34% of its CPU), not the tree exit.
* **The tree exit is proven where the regime actually exists.** On a synthetic
  100 MB monster (400-byte non-periodic space runs) the target-size rule grinds
  `level_elems = 19.42e9` (194× the corpus) to `depth_max = 399`; the rule exits
  it after **one** level (`level_elems = 0.20e9`, −98.9%, `depth_max = 6`),
  CPU −26.8%, wall −3.2% — and the comparator then pays `symbols` ×20.6. The
  amplification is killed; the replacement is not free.

## 1. What landed

| file:line | what |
|---|---|
| `bit6/binbwt_slice.h:679` | `BinEntropyOpts` (armed, `minBits` θ, `leafMinBits`, `leafFloor`) |
| `bit6/binbwt_slice.h:694` | `bin_hist_entropy` — H (bits) of the node's next-byte histogram, from the level's own `count[]`; code 0 (dollar-exhausted) is outside the normalisation, `m − count[0] ≤ 1` ⟹ H = 0 |
| `bit6/binbwt_slice.h:721` | `bin_entropy_exit` — the ONE decision: `H > θ` take, else exit; `entropyEvals/Taken/Exits/ExitRows/NoProgExits`, H journal |
| `bit6/binbwt_slice.h:1548` | `bin_leaf_cmp_sort` — the leaf machine factored out (same bytes, same counters as the pre-change leaf) |
| `bit6/binbwt_slice.h:1623` | `bin_leaf_order` — PART 2, the same rule inside the leaf (`biggest == m` bails to the comparator) |
| `bit6/binbwt_slice.h:1788` | the tree decision in `bin_sort_rec`; `:1800` the exit hands each child to `bin_leaf_order` at the certified depth |
| `bit6/binbwt_slice.h:1970` | the same in the dispatched twin `bin_task_run` |
| `bit6/binbwt_slice.h:3356` | the `BIN_WORK` entropy counters (armed, bits, evals/taken/exits/exit_rows/noprog/leaf arm/over-target) |
| `bit6/binbwt_slice.h:3457` | `BIN_ENTROPY_H` per-depth min/mean/max H journal |
| `bit6/cross_lcp_merge.cpp:5551` | `--bin-entropy-min BITS` (absent = OFF; bare flag = 1.5), `BIN_ENTROPY_LEAF_MIN`, `BIN_ENTROPY_LEAF_FLOOR` |
| `tools/binbwt/oracle.cpp:821` | the ENTROPY pass (7,728 cases: order identity vs off arm + brute SA, degenerate exit, periodic tie-break never lost, θ=0 counter identity, in-leaf arm) |
| `tools/binbwt/entropy_ab.sh` | the A/B driver (probe, one arm at a time, load-labelled) |
| `tools/binbwt/entropy_bank_gate.sh` | the bank gate driver (producer+finish, chi+sha+cmp) |

## 2. The rule, and the counters it keeps

The rule is one line on top of the histogram the level already computed:

```
H = −Σ_{c≥1} p_c log2 p_c          over the live (non-dollar) next-byte buckets
take the level  iff  H > θ          (θ = c_lvl / c_cmp, bits)
```

Measured on the 1 GB product (probe, 96 threads) the H distribution is the
derived threshold's own cross-check — **the mean H crosses 1.5 exactly where the
rule starts exiting**:

```
BIN_ENTROPY_H depth=0  evals=215      h_min=0.0401 h_mean=4.0747 h_max=6.3041
BIN_ENTROPY_H depth=1  evals=18882    h_min=0.0000 h_mean=3.0628 h_max=6.0699
BIN_ENTROPY_H depth=2  evals=275587   h_min=0.0000 h_mean=2.3155 h_max=6.0168
BIN_ENTROPY_H depth=3  evals=1007046  h_min=0.0000 h_mean=1.8956 h_max=6.0348
BIN_ENTROPY_H depth=4  evals=1918812  h_min=0.0000 h_mean=1.6776 h_max=5.9715
BIN_ENTROPY_H depth=5  evals=2484885  h_min=0.0000 h_mean=1.5176 h_max=5.9989
BIN_ENTROPY_H depth=6  evals=2686659  h_min=0.0000 h_mean=1.3986 h_max=5.9055
BIN_ENTROPY_H depth=7  evals=2570077  h_min=0.0000 h_mean=1.3099 h_max=5.9916
BIN_ENTROPY_H depth=8  evals=2100279  h_min=0.0000 h_mean=1.2489 h_max=5.6083
BIN_ENTROPY_H depth=9  evals=1475740  h_min=0.0000 h_mean=1.2228 h_max=5.6430
BIN_ENTROPY_H depth=10 evals=914730   h_min=0.0000 h_mean=1.2174 h_max=5.6406
BIN_ENTROPY_H depth=11 evals=514235   h_min=0.0000 h_mean=1.2086 h_max=5.6143
```

(Caveat, honest: `BIN_ENTROPY_H` aggregates the TREE and the IN-LEAF evals by
depth. Depth 0 is the tree (215 evals of the tree's 370); from depth 1 down the
evals are overwhelmingly the in-leaf arm. The tree's own H is the depth-0 row.)

θ = 1.5 is the middle of the census bracket [1.18, 1.67] (1 GB / 10 GB, design
§2). The measured H says the same thing from the corpus side: at depth 4 the mean
node buys 1.68 bits (take the level), at depth 6 it buys 1.40 (compare) — 1.5
sits on the crossover.

### 2.1 THE CORRECTION THE ORACLE FORCED: H = 0 is broader than `biggest == m`

The design's §4 said `biggest == m` **is** the H = 0 case. It is one shape. Code
0 (the dollar-exhausted rows) is outside the normalisation in
`bin_hist_entropy`, so a node whose **live** rows all share one next byte has
H = 0 while `biggest < m`. The first full oracle run died on its own (too strong)
identity check and the focused reproduction named the shape:

```
IDENT_FAIL f=6 len=64 dollar=1 threads=1 leaf=1          (the 64-'a' fixture)
  off: lv=63 lve=2079 bp=63 cmp=0 sym=0 ls=0 le=0 noprog=0
  on : lv=1 lve=64 bp=1 cmp=270 sym=6983 ls=1 le=63 evals=1 taken=0 exits=1 noprog_exits=1
```

The target-size rule grinds **63 levels** with zero comparisons (one-row leaves);
the rule exits after **one** level into the LCP-carrying comparator (270
comparisons) — the same order, exactly the monster exit, and therefore a counter
difference precisely where a zero-entropy exit happened. The oracle now asserts
what the rule claims: at θ = 0 every exit is a zero-entropy exit
(`entropy_exits == entropy_no_prog_exits`) and the counters are identical to the
off arm where no exit was taken (762 such cases). The design doc §4 carries the
correction inline.

## 3. Gates, verbatim

### 3.1 The fast scratch harness (2,280 cases, 19 fixtures × 2 modes × 2 thread
counts × 3 targets × 5 thresholds × 2 leaf-arm settings), reproduced from this
worktree:

```
SCRATCH_ENTROPY_PASS cases=2280 fixtures=19 exits=8796 taken=21192 zero_entropy_exits=3876
leaf_evals=48406 leaf_taken=42334
```

### 3.2 The full oracle (134 fixtures), one run, exit 0

```
BINBWT_ORACLE_PASS cases=36180 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,4,24 depth_cap=1,4,32 level_bytes=1,2 cap=1,7,all NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic=1 nested=1 single_byte=1 max_byte=1 nonidentity_remap=1 exact_brute_SA=1 multi_window_runs=22500 window_instances=343530
BINBWT_ORACLE_WINDOW_PASS cases=12864 fixtures=134 modes=cyclic,dollar threads=3 level_bytes=1,2 cap=1,7 credit=1,2,3 build_cap=0,5 disc=1 ram=1 file_reuse_runs=11248 window_instances=176784 window_files=88392 exact_brute_SA=1
BINBWT_ORACLE_REFUSAL_PASS refusals=5 modes=unlink,truncate,stale,nomagic,nodone control=exact_brute_SA
BINBWT_ORACLE_STAGE_PASS cases=4824 staged_arms=2412 fixtures=134 modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 stage_hits=42 stage_bytes=297 counters_identical=1 exact_brute_SA=1 both_paths=1
BINBWT_ORACLE_STAGE_PASS cases=4824 staged_arms=2412 fixtures=134 modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 stage_hits=42 stage_bytes=297 counters_identical=1 exact_brute_SA=1 both_paths=1
BINBWT_ORACLE_STAGE_PASS cases=4824 staged_arms=2412 fixtures=134 modes=cyclic,dollar threads=1,3,8 target=1,3,9 depth_cap=0 level_bytes=1 stage_hits=42 stage_bytes=297 counters_identical=1 exact_brute_SA=1 both_paths=1
BINBWT_ORACLE_PERIODIC_SANITY BINBWT_ORACLE_PERIOD_CHILD exact_position_order=1 rows=64 structural_leaves=1 structural_rows=64 probe_off=1
BINBWT_ORACLE_PERIODIC_PASS cases=4824 fixtures=134 modes=cyclic,dollar threads=1,3,8 target=1,4,24 depth_cap=0 (AUTO, no cap) level_bytes=1 cap=7,all periodic_leaves=54 periodic_rows=2304 periodic_period_max=2 over_target_leaves=0 exact_brute_SA=1 window_instances=24822
BINBWT_ORACLE_ADAPTIVE_PASS cases=4016 fixtures=134 modes=cyclic,dollar threads=1,8 target=1,2,3,17 depth_cap=256 level_bytes=1 cap=7,all depth_max=64 leaf_rows_max_le_target=1 over_target_leaves=0 exact_brute_SA=1 window_instances=21736
BINBWT_ORACLE_ENTROPY_PASS cases=7728 fixtures=134 adversarial=4 arms=7 modes=cyclic,dollar threads=1,3 leaf=1,24 theta=0,0.5,1.5,3,8 in_leaf_arm=forced_row_floor_1,off entropy_evals=91838 taken=48512 exits=43326 exit_rows=351534 zero_entropy_exits=7158 leaf_evals=64652 leaf_taken=57232 leaf_exits=7420 over_target_leaves=19418 order_identical_to_off_arm=1 exact_brute_SA=1 degenerate_exit=1576 mixed_fixtures=4 periodic_checked=98 theta0_counter_identical=762 window_instances=1104
BINBWT_ORACLE_FREQ_PASS cases=6432 fixtures=134 modes=cyclic,dollar threads=1,3,8 leaf=1,24 depth_cap=16 level_bytes=1,2 cap=7,all freq_sched=1 exact_brute_SA=1 multi_window_runs=2904 window_instances=33096
BINBWT_ORACLE_PHRASE_PASS cases=672 arms=672 fixtures=7 parse_determinism=7 dictionaries=7 nonempty=6 empty=1 offset_entries_checked=571 offset_entries_lost=4 blocks_addressable=34 blocks_unaddressable=0 probes=385312 skips=13468 skip_rows=81432 skip_bytes=3366000 verify_refusals=0 skip_max_len=64 exact_brute_SA=1 midphrase=1 offsets=1 boundary=1 wrap=1 edit=1 nested=1 norepeat=1 empty_dict_counter_identical=24 modes=cyclic,dollar threads=1,3 leaf=1,4,24 level_bytes=1,2 L=16,20,200,4000 F=2,1000000,2^30 skip_min_rows=1 window_instances=23424
```

The 16 pre-existing PASS lines are **byte-identical to the pre-change oracle's
run** (oracle built from 88a7cb0 before any edit); the only added line is
`BINBWT_ORACLE_ENTROPY_PASS`. The rule is OFF by default and every old gate sees
today's tree, unchanged.

The entropy pass's own case set (what each field means): 7,728 armed cases over
the 134 fixture families + 4 adversarial ones (monster-top, monster-deep, mixed,
periodic-mixed), both modes, threads 1/3, leaf 1/24, θ ∈ {0, 0.5, 1.5, 3, 8}, the
in-leaf arm forced on (floor 1) and off; `order_identical_to_off_arm=1` and
`exact_brute_SA=1` everywhere; `degenerate_exit=1576` cases had a zero-entropy
exit; `mixed_fixtures=4`; `periodic_checked=98` cases required
`periodic_rows(armed) ≥ periodic_rows(off)` (the rule probes the period
unconditionally, so it can only find it sooner).

### 3.3 THE BANKS — byte-identical ON and OFF, both scales

`tools/binbwt/entropy_bank_gate.sh`: real producer (`binbwt --binbwt` with the
product's `CROSS_V2_PIECE_WINDOW=1100`, `--bin-live-cap 4294967296`) streaming
into the real finish (`pile_finish --chi-expect …`), then `sha256` + `cmp` against
the banked container. Walls here are inflated by the producer and the finish
sharing the box (they run concurrently by design).

```text
GATE=100m-off size=100m producer_rc=0 finish_rc=0 cmp=0 chi=29716349 bank_chi=29716349 sha=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5 bank_sha=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5 producer_wall_s=15.08 finish_wall_s=40.56 loadavg_start=76.40 loadavg_end=62.13 extra=none
GATE=100m-on-1.5 size=100m producer_rc=0 finish_rc=0 cmp=0 chi=29716349 bank_chi=29716349 sha=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5 bank_sha=b4753899592c952542f7416ee9ce5836e19b99971afa74d02281493b3a289ba5 producer_wall_s=30.71 finish_wall_s=58.07 loadavg_start=59.64 loadavg_end=48.56 extra=--bin-entropy-min 1.5
GATE=1b-off size=1b producer_rc=0 finish_rc=0 cmp=0 chi=283296933 bank_chi=283296933 sha=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f bank_sha=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f producer_wall_s=180.05 finish_wall_s=300.16 loadavg_start=55.32 loadavg_end=82.02 extra=none
GATE=1b-on-1.5 size=1b producer_rc=0 finish_rc=0 cmp=0 chi=283296933 bank_chi=283296933 sha=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f bank_sha=38f03eb43aaea5997819c8f7922a244a3eb0be945bf1d3b0feb1f2fd9d23144f producer_wall_s=199.22 finish_wall_s=320.60 loadavg_start=83.71 loadavg_end=45.71 extra=--bin-entropy-min 1.5
```

The OFF arm's counters are the banked reference exactly (`level_passes=258`,
`level_elems=217784532`, `comparisons=1864294192`, `emitted_rows=100000503` at
100 MB; `level_passes=648`, `level_elems=2709186546`, `comparisons=21184557983`,
`symbols=223383555397`, `leaf_sorts=62228`, `emitted_rows=1000000665`,
`depth_max=193` at 1 GB) — the refactor and the rule changed no default byte.
The ON arm's producer wall in these runs (30.7 / 199.2 s) is **not** an A/B: the
finish competes for the same cores and the gate boxes carried load 55–84
(§4 is the instrumented A/B). The 100 MB pair's 15.08 → 30.71 s is contention,
not structure: the SAME producer+sink product path run with no finish at all, at
load 21, is `order_s` 3.422 → 3.562 s (off → ON, +4.1%) and `build_cpu_s`
92.7 → 89.7 (−3.2%). The finish itself is unaffected in kind: its wall moves with
the load it shares, and the product it writes is byte-identical.

## 4. The A/B: target-size-only (off) vs entropy rule (ON, θ = 1.5)

Construction-only probes (`--bin-probe`), one arm at a time, **interleaved twice**
(A,B,A,B) so drift cancels; the number quoted is the better of the two runs of
each arm, with the load at the arm's start. `build_cpu_s` is the sum over the
producer's threads; on this box it inflates under load through spin-wait, which
is why the OFF arm's repeats range 1989–2978 CPU-s and the paired run is used.

### 4.1 100 MB, 32 threads (load 65–71 during the paired runs)

| | off | ON θ=1.5 | Δ |
|---|---|---|---|
| `build_s` (wall) | **2.274 / 2.351** | **1.719 / 1.796** | **−24.4%** |
| `build_cpu_s` | 68.2 / 69.1 | 45.6 / 46.0 | −33.1% |
| `leaf_s` (CPU) | 62.6 / 64.5 | 18.6 / 18.7 | −70.3% |
| `comparisons` | 1,948,708,696 | 731,685,432 | −62.4% |
| comparisons/row | 19.49 | 7.32 | −62.4% |
| `symbols` | 15,943,311,434 | 10,327,597,711 | −35.2% |
| `level_passes` | 157 | 2,122,794 | ×13,520 (in-leaf levels) |
| `level_elems` | 185,750,774 | 615,285,796 | ×3.3 |
| `depth_max` | 52 | 33 | |
| `leaf_rows_max` | 386,420 | 788,596 | |
| `entropy_evals / taken / exits` | — | 101 / 92 / 9 | exits = 7,536,584 rows |
| `entropy_noprog_exits` | — | **0** | no H=0 node above target |
| `entropy_leaf_evals / taken / exits` | — | 2,122,693 / 1,605,307 / 517,386 | |
| `BIN_HOT` bin=32 (`0x20`) CPU | 11.6 s (17.0% of 68.2) | 8.56 s (18.8% of 45.6) | −26% absolute, share flat |

The 100 MB win is the **in-leaf arm**: `level_passes` moves from 157 tree levels
to 2.12M in-RAM levels, and the leaf term falls 70%.

**The wall win is thread-count dependent, and the 96-thread arm says why**
(probe, two interleaved passes, load 21–38; and the real product with its sink,
producer only, no finish, load 21):

| 100 MB | off | ON θ=1.5 | Δ |
|---|---|---|---|
| probe `build_s`, 96 threads | 1.250 / 1.271 | 1.625 / 1.672 | **+30%** |
| probe `build_cpu_s`, 96 threads | 91.5 / 93.5 | 87.9 / 89.5 | −2.6% |
| probe `leaf_s`, 96 threads | 81.3 / 82.4 | 23.7 / 23.8 | −71% |
| probe efficiency `cpu/(96·wall)` | 0.76 | 0.57 | −25% |
| product producer+sink `order_s`, 96 threads | 3.422 | 3.562 | **+4.1%** |
| product producer+sink `build_cpu_s`, 96 threads | 92.7 | 89.7 | −3.2% |

At 32 threads the work win dominates (the pool is the binding term); at 96
threads the shorter busy span is spent waiting, at both scales. This is the same
finding as §4.2, not a 100 MB special case.

### 4.2 1 GB, 96 threads (load 65–70 during the paired runs)

| | off | ON θ=1.5 | Δ |
|---|---|---|---|
| `build_s` (wall) | **21.86 / 24.04** | **22.52 / 23.81** | **wash (±10%)** |
| `build_cpu_s` | 1988.7 / 2171.8 | 1442.1 / 1652.3 | −27.5% |
| producer efficiency `cpu/(96·wall)` | 0.95 | 0.67–0.72 | −24…−29% |
| `leaf_s` (CPU) | 1613.4 / 1773.6 | 439.6 / 463.0 | −72.8% |
| `comparisons` | 21,184,557,983 | 9,668,839,544 | −54.4% |
| comparisons/row | 21.18 | 9.67 | −54.4% |
| `symbols` | 223,383,555,397 | 166,803,376,171 | −25.3% |
| `level_passes` | 648 | 16,519,879 | ×25,494 (in-leaf) |
| `level_elems` | 2,709,186,546 | 6,852,105,676 | ×2.5 |
| `depth_max` | 193 | 60 | |
| `leaf_rows_max` | 1,048,394 | 5,922,512 | ×5.6 (the exit's big leaves) |
| `entropy_evals / taken / exits` | — | 370 / 337 / 33 | exits = 117,182,770 rows |
| `entropy_noprog_exits` | — | **0** | the H=0 monster is absent |
| `entropy_leaf_evals / taken / exits` | — | 16,519,509 / 12,270,445 / 4,249,064 | |
| `over_target_leaves` (entropy) | — | 5 | 35.6M rows over target |
| `BIN_HOT` bin=32 (`0x20`) CPU | 387.2 / 417.3 (17.8–21.0% of its run) | 257.5 / 307.3 (17.9–18.6%) | −26…−34% absolute, share ≈ flat |
| `BIN_HOT` bin=32 `level_elems` | 1,028,971,402 | 1,344,928,670 | the bin's rows leave levels for in-leaf radix |
| `BIN_HOT` bin=32 `leaf_sorts` | 23,701 | 7,507,019 | its one big leaf sort becomes in-leaf levels |

**On the banks the H=0 and periodic paths never fire at all**: both scales, both
arms, `no_progress_levels=0` and `periodic_leaves=0` — so the armed arm's
unconditional period probe costs nothing on this corpus, and the question "does
the rule lose the periodic tie-break" is answered by the oracle
(`periodic_checked=98` cases, `periodic_leaves=54`, `periodic_rows=2304`, and the
64-'a' fixture), not by the bank.

**Why the wall does not move.** The producer's work falls by a quarter and its
leaf term by three quarters, but the exits convert 117.2M rows into 33
comparison-sorted leaves, the largest 5.92M rows (vs the target's 1.05M). Those
are serial stretches in a 96-thread schedule: the same CPU is spread over a
shorter busy span but a longer tail, and `cpu/(96·wall)` collapses from 0.95 to
0.67–0.72. The rule is a **work** win at 1 GB, not a wall win. Fixing the tail (splitting
an exit's children into several leaves, or letting the in-leaf arm recurse before
handing to the comparator when `m` is large) is the next lever, not a θ change —
θ = 3.0 in one pass already moved 335M rows into 95 exits and did not help the
wall either (§5).

### 4.3 The degenerate-exit headline, where the regime exists: a synthetic monster

The bank corpus has no H=0 node above target, so the charter's monster mechanism
is priced on a synthetic 100 MB corpus built for it (`/mnt/nvme2n1/erikg/er2-monster/corpus.txt`,
242,131 lines of `AAAA` + 400 spaces + `U######\n`; non-periodic, so the periodic
tie-break cannot finish the run). Probes, 32 threads, contemporaneous runs at
load 22.2 / 22.6:

| | off | ON θ=1.5 | Δ |
|---|---|---|---|
| `build_s` | 197.17 | 190.89 | −3.2% |
| `build_cpu_s` | 269.3 | 197.2 | −26.8% |
| `level_passes` | 402 | 27,619 | |
| `level_elems` | **19,420,843,248** (194× corpus) | **204,842,826** | **−98.9%** |
| `depth_max` | **399** | **6** | |
| `leaf_s` (CPU) | 71.1 | 195.3 | +175% |
| `comparisons` | 2,274,054,500 | 3,243,845,770 | +42.7% |
| `symbols` | 32,085,995,743 | 660,145,138,202 | **×20.6** |
| `entropy_evals / taken / exits / exit_rows` | — | 2 / 0 / 2 / 97,820,924 | one exited node = 97.8M rows |
| `entropy_noprog_exits` | — | 0 | H > 0 but < θ (the H=0 case is the oracle's 64-'a') |
| serial cores busy (`cpu/wall`) | 1.37 | 1.03 | the 0x20 bin is one serial subtree |

This is the mechanism the charter names, measured: **one level instead of 400**,
a 194× level amplification removed (−98.9%), CPU −27% — and the wall barely moves
because the comparator that takes over must walk the 400-byte run per comparison
(`symbols` ×20.6) and the whole `0x20` subtree is serial in both arms. At 100 GB
the same shape would have ground 19.4e12 level rows; that is the "9 h" the design
quotes, and it is exactly what the exit removes. The replacement cost is real and
is the honest limit of the rule as built.

## 5. θ and the in-leaf threshold: the sensitivity curves

### 5.1 θ at 100 MB (32 threads, two interleaved passes, load 115–130)

| θ (bits) | `build_s` | `build_cpu_s` | `leaf_s` | `comparisons` | exits / rows |
|---|---|---|---|---|---|
| off | 2.603 / 3.091 | 77.3 / 91.6 | 72.6 / 84.1 | 1,948,708,696 | — |
| 0.75 | 1.697 / 1.700 | 48.1 / 48.4 | 18.7 / 18.7 | 625,746,515 | 3 / 2.2M |
| **1.5 (default)** | 1.760 / 1.786 | 48.6 / 47.7 | 21.6 / 21.2 | 731,685,432 | 9 / 7.5M |
| 3.0 | 2.081 / 2.049 | 50.2 / 49.5 | 26.8 / 26.3 | 898,529,415 | 30 / 23.8M |
| 8.0 | 10.475 / 11.169 | 53.1 / 57.2 | 38.3 / 40.2 | 1,353,337,552 | 29 / 89.0M |

θ is **flat between 0.75 and 1.5** and degrades slowly to 3; at 8 the exits
(89M rows) destroy the schedule and the wall explodes ×4. The default 1.5 is
inside the flat optimum.

### 5.2 θ at 1 GB (96 threads; single passes with rising load 41→93; the paired
off/1.5 runs of §4.2 are the reliable comparison)

| θ (bits) | `build_s` | `build_cpu_s` | `leaf_s` | `comparisons` | exit rows |
|---|---|---|---|---|---|
| off | 35.800 | 2978.8 | 2330.6 | 21,184,557,983 | — |
| 0.75 | 29.698 | 1962.7 | 397.4 | 8,154,662,977 | 41.7M |
| 1.5 | 30.969 | 1791.1 | 477.6 | 9,668,839,544 | 117.2M |
| 3.0 | 32.626 | 1539.3 | 531.8 | 11,810,962,241 | 334.8M |
| tree-only 1.5 | 48.447 | 2559.2 | 1795.4 | 21,911,785,474 | 117.2M, 28 over-target leaves |

All θ in [0.75, 3] are the same story (work down, wall flat), inside the noise of
the rising load. **The tree arm alone is worse** (+35% wall, `leaf_rows_max`
21.2M, 28 over-target leaves): handing rows to the comparator without the in-leaf
arm underneath is the bad half of the rule on this corpus.

### 5.3 The in-leaf threshold (tree θ fixed at 1.5)

100 MB, 32 threads (load 46–52), `BIN_ENTROPY_LEAF_MIN`:

| leaf θ | `build_cpu_s` | `leaf_s` | `comparisons` |
|---|---|---|---|
| 0 | 51.8 | 12.5 | 425,888,664 |
| 0.1875 | 50.4 | 17.4 | 625,746,515 |
| **0.375 (default, 1.5·0.25)** | **50.3** | **19.9** | 731,685,432 |
| 0.75 | 51.5 | 24.3 | 893,478,463 |
| 1.5 | 55.3 | 32.2 | 1,152,960,696 |
| 3.0 | 64.7 | 48.2 | 1,598,810,369 |
| off (no leaf arm) | 79.6 | 69.3 | 1,995,191,038 |

1 GB, 96 threads (load 52–67):

| leaf θ | `build_cpu_s` | `leaf_s` | `comparisons` | `build_s` |
|---|---|---|---|---|
| 0 | 2202.3 | 244.9 | 4,819,348,465 | 39.764 |
| **0.375 (default)** | 1945.5 | 496.9 | 9,668,839,544 | 35.161 |
| 0.75 | **1869.4** | 560.6 | 11,718,956,277 | 32.306 |
| 1.5 | 1970.7 | 909.1 | 14,564,659,430 | 37.303 |
| 3.0 | 2046.7 | 1335.1 | 19,083,043,807 | 39.761 |
| 1e9 (disabled) | 2175.5 | 1672.4 | 21,911,785,474 | 38.841 |
| off | 2978.8 | 2330.6 | 21,184,557,983 | 35.800 |

The in-leaf threshold has a real interior optimum: ~0.375 at 100 MB, ~0.75 at
1 GB. The shipped ramp (θ·0.25 = 0.375) lands inside the flat optimum at both
scales, and is within 4% of the 1 GB optimum's CPU. The ramp is a heuristic, not
a derivation — see the risks.

## 6. Reconciliation with `fp-lce`

`fp-lce` (branch `fp-lce`, tip 2acea4c) has landed its report
(`tools/binbwt/RESULTS_FP_LCE.md`): the fingerprint ladder answers the **same**
LCEs (`symbols` bit-identical to the byte-walk) and the producer gets **slower**
(the census ceiling confirmed — the leaf comparator's 2 random windows per
comparison is what dominates, and a hash probe replaces a prefetcher-served
sequential window with a random one).

The two charters attack the same term from opposite directions, and the numbers
here are the cross-check of theirs:

* their census constants are my OFF arm exactly — 1 GB `comparisons=21,184,557,983`,
  `symbols=223,383,555,397`, `windows=22,626,622,708` — so θ's derivation inputs
  (c_lvl 112.1 ns, c_cmp 67.1 ns, 57 ns/window) are reproduced on this tree.
* `fp-lce` changes **what one comparison costs** (windows −4.9%, wall +2.7 s);
  the entropy rule changes **how many comparisons happen** (`symbols` −25.3%,
  `comparisons` −54.4%) by spending cheap RAM levels instead.
* Neither is a wall win at 1 GB on this box for the same reason: the leaf term is
  already 83–93% of build CPU and both arms move it on the CPU axis, not the
  occupancy axis. If the sibling's calibration sweep lands a window-cost constant
  different from 57 ns, θ moves with `c_cmp` (θ = c_lvl/c_cmp); nothing else in
  the rule depends on that number.

## 7. Residual risks

1. **The wall win only exists at low thread counts.** At 1 GB/96 threads the
   exits create serial leaves (5.92M rows, `over_target_leaves=5`,
   `leaf_rows_max` ×5.6) and producer efficiency falls 0.95 → 0.67–0.72, so a
   −27.5% CPU win is a wall wash. At 100 MB it is a −24% wall win at 32 threads
   and a +30% probe / +4% product wall LOSS at 96 threads, same cause. Any claim
   that this rule speeds up the producer at 96 threads is FALSE as built.
2. **The H=0 monster exit is unexercised on the real corpus at 1 GB**
   (`entropy_noprog_exits=0`). It is proven on the oracle (7,158 zero-entropy
   exits, 1,576 degenerate cases) and on the synthetic monster (the H<θ exit:
   level rows −98.9%), but the bank corpus's `0x20` cost is leaf sorting. The
   100 GB "9 h" claim is therefore inferred from the mechanism, not reproduced.
3. **The exit's replacement cost is real.** On the synthetic monster the
   comparator walks the run: `symbols` ×20.6, leaf CPU +175%, wall −3.2%. The
   cost model's `c_lvl` assumes cold random level rows (2×57 ns); with the corpus
   RAM-cached the level row measures ~10 ns (19.4e9 rows in 193 CPU-s), which is
   why the level grind was cheap per row and the rule's win is CPU, not wall.
4. **The in-leaf ramp (0.25) is a heuristic.** The measured optimum is ~0.375 at
   100 MB and ~0.75 at 1 GB (CPU within 4% of the default); the in-leaf arm reads
   through `CorpusText` and is NOT stage-backed, so on a corpus that does not fit
   the RAM cache each in-leaf level row is 2 window reads and the ramp is
   unpriced. Re-measure at 10/100 GB with the cache off before trusting it there.
5. **θ is machine- and scale-dependent** ([1.18, 1.67] measured). The sweep says
   0.75–3 is flat at 100 MB and a wash at 1 GB, so the default is safe; a
   different box (or a cold-cache corpus) moves the optimum.
6. **Instrumentation caveat**: `BIN_ENTROPY_H` mixes tree and in-leaf evals by
   depth; `level_passes`/`level_elems` include in-leaf levels, so they are not
   comparable to the old tree-only numbers (this is why they grow 13,000×).
7. **Timing noise on a loaded box**: `build_cpu_s` inflates under load via
   spin-wait (OFF arm repeats: 1988.7–2978.8 CPU-s). Only interleaved pairs are
   quoted as deltas; every wall carries its load label.

## 8. Branch and commits

Branch `entropy-rule` (worktree `pi-worktree-ab83d96f-…`), base
`pile-week-v1 @ 88a7cb0`.

| commit | what |
|---|---|
| `b80e97b` | Phase 0 design: the cost model, θ DERIVED from the census (1.18–1.67 measured, 1.5 adopted) |
| `d7c5e68` | the rule: H from the level's own histogram, one decision per node, the periodic tie-break as the H=0 special case, the in-leaf arm, `--bin-entropy-min` |
| `63fac47` | the theta=0 oracle check corrected (H=0 is broader than `biggest == m`, the 64-'a' fixture proves it) + the two A/B drivers |
| `91100fa` | the full oracle green with the entropy pass, all 16 pre-existing passes byte-identical to the pre-change oracle |
| `…` | this results document |

Scratch (all on the nvme): `/mnt/nvme2n1/erikg/er2-{build,oracle,gate,ab100,ab1b,monster,repro,scratch}`.
