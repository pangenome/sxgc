# RESULTS — sampled SA (option B): the measured curve

Branch `sampled-sa`, base `pile-week-v1 @ aa1a95c`. Design + audit:
`tools/binbwt/SAMPLED_SA_DESIGN.md`. Everything here was run through the REAL
bundled binaries (`cross_lcp_merge --binbwt` + `pile_finish`), against the
BANKED containers, on a box that was carrying the live 100 GB rung
(`/mnt/nvme2n1/erikg/ladder100`, 96 threads) throughout. **Every wall below is
labelled with the load average it ran under; none of them is a quiet-box
number.** The 10 GB rung was NOT attempted: `pgrep -f ladder100` shows the 100 GB
rung live, and the charter forbids touching it.

## 1. The gates (Phase 3/4 correctness)

### 1a. The tiny brute-force oracle (`sa_anchor_oracle`, verbatim)

```
SA_ANCHOR_NEGATIVE_PASS corrupted_entry_refused=1 exit=2
SA_ANCHOR_ORACLE_PASS arms=88 files=102 anchor_files_entries=1754 corpora=7 k_sweep=1,2,3,7,>=n arms=per-row+publish negative_arm=1
```

7 adversarial corpora (64 random bytes over a 6-symbol alphabet, all-equal,
periodic `abab…`, separators + repeats, n=1, n=2, run-mixed) × dollar/cyclic ×
k ∈ {1, 2, 3, 7, ≥n}, on **both** emit arms (the per-row lambda sink and the
`bin_publish_sink` whole-bin publisher), each anchor file checked entry by entry
against the brute-force suffix array. `k=1` must reproduce the **full SA** (the
sledgehammer); `k≥n` must emit exactly `SA[0]`. The negative arm corrupts one
entry on disc and requires the checker to die (exit 2) — a gate that cannot fail
proves nothing.

### 1b. The fixture-scale verifier (`sa_anchor_verify`, 100 MB, k=1 = the sledgehammer)

```
SA_VERIFY_ORDER ok=1 anchors=100000503 k=1 ranks_covered=100000503
  first_symbol_ascents=207 equal_first_symbol=100000295
SA_VERIFY_FULL ok=1 ranks=100000503 permutation=1 strictly_increasing=1
  adjacent_compare_symbols=4093670730 mean_lcp=40.937 max_lcp=34067
SA_VERIFY_PASS mode=full-sa anchors=100000503 k=1
VERIFY_WALL=23.61 maxrss_kb=893024
```

The `k=1` anchor file of the 100 MB fixture **is a valid suffix array** of that
corpus, verified independently of the producer: it is a permutation of [0, n),
its suffixes are strictly increasing (4.09e9 symbol comparisons of adjacent
pairs over the remapped corpus), mean adjacent LCP 40.9, max 34,067. The
sledgehammer statement is checkable, and it is checked.

## 2. THE SWEEP: 100 MB fixture (`mergperf-100m-snap.txt`)

Producer `--threads 96`, finish `--threads 96 --chi-expect 29716349`, container
`cmp` against the banked `xsa-pw3-chunk-100m/mergperf-100m-snap.sxi2`.

| k | anchor bytes | anchor entries | producer wall | finish wall | total | cmp vs bank | finish verified_symbols | verdict |
|---|---|---|---|---|---|---|---|---|
| 0 (OFF) | 0 | – | 4.25 s | 14.79 s | 14.8 s | identical | 1,176,027,277 | PASS |
| 4 | 200,001,056 | 25,000,126 | 4.80 s | 18.74 s | 18.8 s | identical | 1,176,027,277 | PASS order-ok |
| 16 | 50,000,304 | 6,250,032 | 7.30 s | 23.00 s | 23.0 s | identical | 1,176,027,277 | PASS order-ok |
| 64 | 12,500,112 | 1,562,508 | 5.77 s | 16.84 s | 16.9 s | identical | 1,176,027,277 | PASS order-ok |
| 256 | 3,125,064 | 390,627 | 4.33 s | 15.92 s | 15.9 s | identical | 1,176,027,277 | PASS order-ok |
| 1024 | 781,304 | 97,657 | 6.85 s | 19.20 s | 19.2 s | identical | 1,176,027,277 | PASS order-ok |

loadavg at the start of each row: 246.7, 236.5, 250.2, 274.7, 271.7, 255.7. The
producer wall is **flat within the load noise** across two orders of magnitude of
k, and the same is true at 1 GB (§3): the side-stream is inside the machinery's
own jitter. The one attributable number is the write itself, measured apart:

```
100 MB, k=1:  SA_ANCHOR_DONE k=1 entries=100000503 bytes=800004072 write_s=0.984
```

= **813 MB/s of sequential pwrite** for a fully materialised SA (8 B/row). A k=64
file is 1/64 of that.

## 3. THE SWEEP: 1 GB fixture (`mergperf-1b-snap.txt`)

Producer `--threads 96`, finish `--chi-expect 283296933`, container `cmp` against
the banked `pilev2-g2-1b/frag.sxi2`.

| k | anchor bytes | anchor entries | producer wall | finish wall | total | cmp vs bank | finish verified_symbols | verdict |
|---|---|---|---|---|---|---|---|---|
| 0 (OFF) | 0 | – | 179.03 s | 229.11 s | 229.1 s | identical | 12,820,424,563 | PASS |
| 64 | 125,000,136 | 15,625,011 | 162.65 s | 211.45 s | 211.5 s | identical | 12,820,424,563 | PASS order-ok |
| 1024 | 7,812,560 | 976,564 | 156.70 s | 204.43 s | 204.4 s | identical | 12,820,424,563 | PASS order-ok |

loadavg at the start of each row: 266.4, 283.8, 242.1. The banked counter
reproduced exactly (`queries=473,695,414 verified_symbols=12,820,424,563`, the
number `RESULTS_FINISH_DECOMP.md:147` banks) — so these are the same runs as the
standing gate, with the knob on.

Anchor sizes match `8n/k + 48` to the byte: 15,625,011 × 8 + 48 = 125,000,136;
976,564 × 8 + 48 = 7,812,560.

### 3a. The k=1 full-SA verification at 1 GB

```
(§4 below - the run completed after this table was written)
```

## 4. THE CURVE, READ HONESTLY: "placement cost vs sampling constant"

Two columns of the tables above answer the operator's question and they disagree
with the charter's expectation, so they are stated together:

* **Cost vs k is exactly `8n/k` bytes** (measured, to the byte, at every k on
  both fixtures) plus a write that is inside the wall noise.
* **Benefit vs k is zero verified symbols**, measured: the finish's
  `verified_symbols` counter is *bit-identical* — `1,176,027,277` at 100 MB and
  `12,820,424,563` at 1 GB — for k ∈ {0, 4, 16, 64, 256, 1024}. The container is
  byte-identical too.

That is the design doc's §2 theorem confirmed by measurement rather than argued:
the finish's 473,695,414 queries are `LCP(a,b)` between suffixes whose **ranks are
already known** (BWT run endpoints), so an SA anchor (rank → SA value) carries no
information about the answer. `cost(k) = base + 8n/k` with a zero benefit term has
no interior optimum, and the sweep's monotone-decreasing cost column is the whole
story: **the knob's best setting for this consumer is off.**

The residual walk that the charter wanted to shorten (`mean 27 → residual`) does
not exist on this data: mean verified prefix is 27.06 symbols and the *anchors
are not on the path*. What the numbers say instead:

The finish's own counter line from the k=0 run (verbatim, 1 GB):

```
SLIM_FP pile-finish-corpus tau=22 bytes=363636608 queries=473695414
  verified_symbols=12820424563 hash_probes=669260274 mean_verified=27.064700616
  max_verified=97060 max_requested_lce=97059 verification_work=14816771223
  hash_probe_charges=2677041604 suffix_symbol_reads=28110010031
  total_work=17493812827 total_limit=128000085120
SWEEP_PHASE_WALLS repair=130.2 lce=165.0 ...      (this box, load ~266)
PILE_FINISH_REGION lce-span wall=165.011 waited=11.642 overlapped=153.369
```

| quantity | measured |
|---|---|
| finish queries at 1 GB | 473,695,414 = **1.287 per BWT run** over 368,036,610 runs |
| verified symbols at 1 GB | 12,820,424,563 = **12.8 per corpus byte** |
| mean verified prefix | 27.064700616 symbols (`max_verified=97060`) |
| hash probes / symbol reads | 669,260,274 / 28,110,010,031 |
| total_work vs the 128n policy budget | 17,493,812,827 / 128,000,085,120 (13.7%) |
| LCE phase wall | 165.0 s (load ~266; banked 154.8 at load ~120) |
| family F1 (`topLcp`) | 1 per run, ADJACENT SA ranks = LCP array entry r |
| family F2 (`interior`) | ~0.287 per run (the excess over 1/run), min LCP over a mean-1.7-entry interval |
| the container's OWN SA sample | 2 per run (head+tail packed to `bits(n+9)`) = 736,073,220 values, **mean spacing 1.36 ranks** |

So the SA is already sampled *twice as densely as the densest k in the charter's
sweep* — the sample is not the missing structure.

## 5. THE PILE RECOMMENDATION

### 5.1 The sampled SA (option A as chartered)

At pile scale (`n = 1.31e12`, so `8n/k` = 10.5 TB / k):

| k | anchor disc | verified symbols saved | verdict |
|---|---|---|---|
| 64 | 164 GB | 0 | no |
| 256 | 41 GB | 0 | no |
| 1024 | 10.2 GB | 0 | **still no** |

**Recommended k for the pile: OFF.** The disc cost is affordable at k ≥ 256 and
that is not the problem — the benefit is zero at every k, so there is no k where
it amortises. If an SA sample is wanted for a *future* consumer (the r-index
locate path needs `SA[rank]` at an interval boundary), the container already
carries it at 1.36-rank spacing, so even k=1024 adds nothing but 10.2 GB of
redundancy. Keep the emission hook (it is ~free and it is the only way to get SA
values out of the producer without a persistent column); do not build on it.

### 5.2 What WOULD collapse the finish: the run-boundary LCP column

| | 1 GB (measured census) | pile (r/n = 0.368 projection) |
|---|---|---|
| column | 2 B/run (`u8` + deep-value escape list) | same |
| bytes | 736 MB | ~0.97 TB |
| finish verified symbols | 12.82e9 → **0** | 1.68e13 → **0** |
| finish LCE wall | 165.0 s of a 229 s finish / 737 s end-to-end banked (21%) → ~0 | ~9 h of the projected 43.3 h finish → ~0 |
| producer cost | one Kasai pass, **O(n) total symbol comparisons (~2n)**, versus the 12.8n the finish spends now | same ratio |

The constant that makes "sampling" worth it is therefore **not a k, it is a
different column**: 0.74n bytes of LCP at the run boundaries, computed by the
`O(n)` amortised construction the finish does not have (it pays `O(answer)` per
query, un-amortised, 473.7M times). That is the recommendation this survey
produces, and it is priced, not implemented — it is a charter of its own
(producer `O(n)` pass + one piece column + swapping `slimLce(...)` for a table
read in `pile_finish.cpp:3505-3506`).

## 6. Residual risks

* **The box was never quiet**: loadavg 236–310 with the live 100 GB rung on 96
  threads. The producer/finish walls in §2/§3 are ceilings and are not
  comparable with the banked quiet-box figures; only the COUNTERS (identical
  containers, identical verified_symbols, anchor sizes to the byte) are hard.
* `R` at pile is projected from the 1 GB fixture; deep repeats make `R` smaller,
  so §5.2's 0.97 TB is an over-estimate in the direction that helps.
* §5.2's "21% of the finish wall" is the 1 GB LCE share measured at load ~120
  (`RESULTS_FINISH_DECOMP.md`); on a cold pile corpus the LCE walks are
  device-read bound as well, so the share is likely *larger*, not smaller.
* The `u8` fast path for the LCP column assumes the deep tail stays sparse (1 GB
  `max_verified` = 97k-class). A heavier pile census grows the escape list, not
  the dense column.
* The anchors are verified at fixture scale by (a) the brute oracle at tiny
  scale, (b) `--full` at 100 MB, (c) the non-decreasing-first-symbol check at
  1 GB. There is **no** independent full-SA oracle at 1 GB except the k=1 run in
  §3a; k ≥ 2 files are checked against the corpus projection only, which is
  necessary but not sufficient.
