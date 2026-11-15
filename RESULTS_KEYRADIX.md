# feat/keyradix-leaf-sort — the leaf sort, made scale-invariant

Branch `feat/keyradix-leaf-sort` from `origin/main` @ `5e13d3d`.
Sorter under change: `xsa/runtime/tools/binbwt/scatter_sort.cpp` (the source of
truth; `tools/binbwt/scatter_sort.cpp` is the stale snapshot and was not touched).

## The mechanism

`radix_sort_band()` — `scatter_sort.cpp:702` (dispatch `:3412`):

* **PHASE 1** (`:668`) orders the band by the 24-byte key that is already IN the
  row. A three-word comparison sort; the LCP of any pair that differs inside the
  key is `clz` of the xor, exact and free. A band whose keys are all equal (the
  deep-band case, `d_known >= 24`) skips the sort entirely. Zero corpus reads.
* **PHASE 2** (`:719`) takes the tie groups. `extract_key_windows()` (`:571`)
  does ONE forward pass over **ascending positions**, reading bytes
  `[d_known + 24 + off, +K)` into an arena key plane. Clusters merge only across
  a gap of at most one page, so a dense bucket is one sequential pread and a
  sparse one is one page per row. `msd_level()` (`:553`) partitions with
  8-bit-digit MSD radix over ping-pong row planes; ties a window cannot separate
  extract the next window (K, 2K, 4K, …) and are partitioned again. A span of at
  most 64 rows is ordered by direct comparison instead of a 257-bucket radix.
* **THE DIGIT SPACE IS 257, NOT 256.** Bucket 0 is the virtual END, which sorts
  below every symbol — dollar mode exactly. That bucket is provably a singleton
  (two distinct positions never have the same suffix length), which is what makes
  the round loop terminate.
* **THE LCP COLUMN IS STILL FREE.** An MSD split at digit `g` proves the adjacent
  pair across the split has LCP exactly `g`; boundaries only refine, so the stamp
  is the final value. `lcpv[k]` is that stamp. The round cap's leftovers go to the
  **LCE fallback** (`:839`), which walks from the window end (`off`), not byte 0.
* Knobs: `--sort-key-bytes` / `XSA_SORT_KEY_BYTES` (K, default 32, CLI wins),
  `--sort-rounds` / `XSA_SORT_ROUNDS` (cap, default 5 ⇒ 1016 bytes covered),
  `--leaf-sort` / `XSA_LEAF_SORT` = `radix` (product) | `lce` (the pre-change arm,
  kept as the A/B lever and oracle). `SS_LEAF_SORT` journals the arm and price.
* `bytes_per_row` 90 → 126 (2 row planes 64 + lcpv 8 + column 1 + esc 9 + key
  plane 32 + slot planes 8 + ping-pong slot 4), honestly: the merge arm's two
  u64 LCP planes are gone and the key plane is capped, which is what keeps the
  leaf's price DATA-independent (a fully-tied band gets a smaller window and more
  rounds, never a bigger arena).
* **Plan-invariance, measured**: two runs at `rows_per_band` 92,773 and 2,743,902
  (30x apart) emit byte-identical `out.sa` and `out.lcp`, so the container cannot
  depend on the leaf's memory footprint — which is what makes the 126-vs-90
  change safe for any gate that compares containers built from different budgets.

## Gates

Harness: `gates_keyradix.sh` (every run wrapped in `setsid timeout --kill-after=10`,
with a survivor assertion). Pre-change binary: `/tmp/pre-keyradix/scatter_sort`,
compiled with the same `g++ -O2 -std=c++17 -pthread … -latomic` from
`git show origin/main:xsa/runtime/tools/binbwt/scatter_sort.cpp`.

### K2 — brute oracle on the adversarial fixtures (all green)

7 fixtures × 6,291,456 B, **44,040,199 rows examined**; `sa_check` (permutation +
exact adjacent order) PASS on every one, the brute-force `subdiv_oracle` (n log n
string comparisons) PASS with `mismatches=0` on every one, and `out.sa` **and**
`out.lcp` are byte-identical to the pre-change binary on every one.

```
KEYRADIX_K2_SUMMARY fixtures=7 rows_examined=44040199 all_green=1
KEYRADIX_K2_ORACLE fixture=<each> SUBDIV_ORACLE_PASS rows=6291457 mismatches=0 (brute-force sort, virtual END rank 0)
KEYRADIX_K2_SA_CHECK fixture=<each> SA_CHECK_PASS entries=6291457 dollar=1
```
Fixtures (from `gates/subdiv_fixtures.c`): `single` (one byte to EOF), `period2`
(`ab`*N), `eofrun` (maximal one-symbol tail run), `docsep` (0x1E boundaries),
`internsep` (0x1E inside words), `mixed` (several alphabets), `r0` (all-distinct
tail neighbourhood).

### K4 — tie accounting (per-round receipt)

Per-round journal, verbatim from `single.txt` / `period2.txt`:
```
SORT_ROUND band=3 round=1 off=56 k=32 rows_in=149263 rows_tied=4043 bytes_read=6291042 syscalls=3 shrink=1
SORT_ROUND band=3 round=2 off=120 k=64 rows_in=4043 rows_tied=3977 bytes_read=4232 syscalls=3 shrink=1
SORT_ROUND band=3 round=1 off=56 k=32 rows_in=146464 rows_tied=2021 bytes_read=6291352 syscalls=3 shrink=1
SORT_ROUND band=3 round=2 off=120 k=64 rows_in=2021 rows_tied=1989 bytes_read=4040 syscalls=1 shrink=1
SORT_ROUND band=3 round=3 off=248 k=128 rows_in=1989 rows_tied=1925 bytes_read=3976 syscalls=1 shrink=1
SORT_ROUND band=3 round=4 off=504 k=256 rows_in=1925 rows_tied=1797 bytes_read=3848 syscalls=1 shrink=1
```
```
KEYRADIX_K4 fixture=single      rounds_max_seen=5 stall_rounds=0 tie_rows_sum=4145384 ext_bytes_sum=163504674 fallback_rows_sum=6162
KEYRADIX_K4 fixture=period2     rounds_max_seen=5 stall_rounds=0 tie_rows_sum=4125760 ext_bytes_sum=163544904 fallback_rows_sum=6162
KEYRADIX_K4 fixture=eofrun      rounds_max_seen=5 stall_rounds=0 tie_rows_sum=4421772 ext_bytes_sum=163045964 fallback_rows_sum=63564
KEYRADIX_K4 fixture=docsep      rounds_max_seen=2 stall_rounds=0 tie_rows_sum=4109894 ext_bytes_sum=163571442 fallback_rows_sum=0
KEYRADIX_K4 fixture=internsep   rounds_max_seen=2 stall_rounds=0 tie_rows_sum=3730192 ext_bytes_sum=163570362 fallback_rows_sum=0
KEYRADIX_K4 fixture=mixed       rounds_max_seen=3 stall_rounds=0 tie_rows_sum=9542168 ext_bytes_sum=324259712 fallback_rows_sum=0
KEYRADIX_K4 fixture=r0          rounds_max_seen=5 stall_rounds=0 tie_rows_sum=4145498 ext_bytes_sum=163508186 fallback_rows_sum=6162
KEYRADIX_K4_ROUNDS total=… stalled=0
```
(a) no band exceeded the cap (max 5, cap 5); (b) **every** round has `shrink=1`
and `stalled=0`, i.e. every round that produced survivors produced FEWER of them;
(c) the tie work is proportional to the tie rows, not to n: `ext_bytes /
tie_rows` = 39.4–44.4 bytes per tie row against K = 32 per round, and the
`SS_SORT_IO` receipt at 6.3 MB is `tie_bytes_per_tie_row=32..104`,
`ext_bytes_per_row=13..26`. At 150 MB: `tie_rows=40,766,164
tie_bytes=3,398,879,880 ext_bytes=15,136,777,870 ext_syscalls=4,538,245
fallback_rows=1,264,337 no_shrink_rounds=0 tie_bytes_per_tie_row=83.38
ext_bytes_per_row=100.91`.

### K5 — no regression

* `cargo test --release`: **106 passed; 0 failed; 1 ignored**.
* 150 MB (`pile150.txt`, 150,000,001 B): `out.sa` and `out.lcp` **byte-identical**
  to the pre-change binary, 307 bands compared.
  ```
  KEYRADIX_K5_150M sa=728d506ef64a88f488a1e329ec21a3d4bdcf8d314785371d7af40ddbd48413db (both)
  KEYRADIX_K5_150M lcp=dacd9d3996a9e976cf3fdb508cd828085da8cd278a2e7014bd6f88712afb01d6 (both)
  ```
* 10 GB (`pf-10gb.txt`, n = 10,000,000,001): `out.sa` and `out.lcp`
  **byte-identical** to the pre-change binary.
  ```
  new  sa=a37dbe2a3d1fbb99ec2bf17f821fdb0d284c0279b92d89a5de7a5cc5c6efc3db lcp=01d6ce6a094bfaed9676de2e561ba1c95c1783a270e2754f30add7d1b445bf1c
  base sa=a37dbe2a3d1fbb99ec2bf17f821fdb0d284c0279b92d89a5de7a5cc5c6efc3db lcp=01d6ce6a094bfaed9676de2e561ba1c95c1783a270e2754f30add7d1b445bf1c
  ```

### K1 — 100 GB identity: NOT RUN (and why, with numbers)

The pre-change arm needs a full 100 GB sorter pass, and it costs log2(rows_per_band)
× a suffix walk per row. MEASURED on `pile-head100.txt` with
`--threads 24 --budget-bytes 48e9`: **wave 32 of 249 after 2 h 38 m**, `band_sum
sort` 989 s for one wave's 15 bands ⇒ ≈ **34 h for one pass**. Two arms is ≈ 68 h,
which this lane does not have, and running it would also fight the operator's live
full-PILE build on the same box.

What stands in its place, and what it does and does not cover:
* identity at **1.0e10 rows** (10 GB) and 1.5e8 rows (150 MB) and 7 × 6.3e6 rows —
  all byte-identical, including the LCP column;
* the *mechanism* is corpus-size-independent: the only thing scale changes is how
  many rounds a tie set needs and how sparse the positions are, and both are
  exercised harder by the deep bands at 10 GB (249-wave equivalent structure) than
  by the fixtures;
* plan-invariance (above), so a 100 GB comparison could not fail for a budget reason.

**First-differing-band reporting is implemented and wired** in
`gates_keyradix.sh k1-100g`: on a mismatch it prints
`KEYRADIX_K1_100G FIRST_DIFF_BYTE=… first_diff_rank=…` and stops; it does not
iterate. It resumes from an existing `out.sa`, so the base arm can be built in
the background and the gate re-run later without redoing the arm that finished.

### K3 — bytes of disc read per row

Two measurements, both on corpora and flags held identical between arms.

**(a) 10 GB, both arms, whole run, the kernel's own ru_inblock** (`/usr/bin/time %I`,
512-byte blocks; `Cached` ≈ 998 GB of 1 TB at start, so the 10 GB corpus is largely
resident and this is the *charitable* case for the pre-change arm):

| arm | wall | disc blocks read | bytes/row (n = 1.0e10) |
|---|---|---|---|
| pre-change (LCE comparisons) | 1006.47 s | 14,022,048 | **0.718** |
| keyradix (streaming extract) | 1539.89 s | 936 | **4.8e-5** |

ratio: **≈ 15,000x fewer bytes pulled back off disc per row**. The mechanism is
visible in one number: the comparison sort's random walks keep evicting the
corpus it is comparing through, and the ascending extraction does not.

**(b) the leaf sort's own corpus traffic at 10 GB** (`SS_SORT_IO`):
`tie_rows=4,294,356,835 tie_bytes=280,396,322,453 ext_bytes=2,294,078,365,545
ext_syscalls=1,062,780,043 fallback_rows=237,761,301 tie_bytes_per_tie_row=65.29
ext_bytes_per_row=229.41`. 229 bytes of *requested* corpus read per row of a
10 GB build, against the mechanism it replaces: log2(m) ≈ 22 comparisons per row
of tie set, ~2 page touches each, i.e. ~44 page touches/row — the operator's
measured 1,400x-per-row cliff at 1.31 TB.

**(c) pages/row is the floor, and the first version was 60x worse.** The
extraction's cluster rule is the whole difference, and it was measured: at 10 GB
the span-based rule read **32.5 GB** for one 4.8 M-row band (278 keys per 2 MiB
window); the gap-based rule reads **2.3 GB** for the same band.

## Wall clock, labelled

`uptime` during the 150 MB measurements: load average 42.9 (another lane running a
FastGA aligner on ~22 cores and an `xsa_finish` at 11 GB RSS; the same box also
runs the operator's full-PILE build).

| corpus | pre-change | keyradix | note |
|---|---|---|---|
| 150 MB sorter pass | 18.73 s | 27.66 s | corpus fully resident; the comparison arm's walks are cache hits |
| 10 GB sorter pass | 1006.47 s | 1539.89 s | both `--threads 24 --budget-bytes 48e9`, run back to back on the same box |

**This is a wall regression in the cache-resident regime, and it is reported as
one.** The leaf's win is in *bytes pulled off disc per row*, which is what the
non-resident regime is made of; when the whole corpus is in RAM the comparison
sort's walks cost nothing and this arm's fixed costs (a u32 slot plane per round,
a key plane, ~0.25 preads per tie row) are pure overhead. Phase breakdown at
150 MB (`SORT_ARM`, summed over band workers): phase1 14.0 s, extract 41.9 s,
radix 11.7 s, fallback 15.5 s, column 0.4 s. Before the small-span fix the radix
phase alone was 459 s and the pass was 54 s.

## Residual risks

1. **100 GB container identity is unmeasured** (see K1). The identity evidence
   stops at 1.0e10 rows.
2. **Wall regression when the corpus is cache-resident** (1.5x at 10 GB and 150 MB).
   The 10 GB banked case would get slower. Judgement call, not a measurement gap.
3. `bytes_per_row` 90 → 126 shrinks `rows_per_band` ~28% wherever the ARENA binds
   instead of the scratch window. Measured safe for the emitted bytes (plan
   invariance) but it changes the band cut points, hence any per-band accounting.
4. The round cap (5) bounds the tie bytes; a genuinely 1000-byte-deep tie set
   beyond that falls to the LCE arm for the residual only, which is bounded and
   journaled (`fallback_rows`) but is still random I/O.
5. `--leaf-sort=lce` reproduces `origin/main` byte-for-byte, so the A/B lever is
   honest; but nothing in the product route sets it, so it is a gate lever only.
6. Round termination is clean on the final binary: the 150 MB build journals
   `SS_SORT_IO ... no_shrink_rounds=0` and zero `SORT_ROUND_STALL` lines, i.e. every
   round that produced survivors produced fewer of them. 1,264,337 rows (0.84% of
   n) still reach the LCE fallback on prose and are journaled.
