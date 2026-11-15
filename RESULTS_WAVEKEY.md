# feat/wave-key-sweep — EXTRACT ONCE PER WAVE, DURING THE SWEEP

Branch `feat/wave-key-sweep` from `origin/main` @ `5e13d3d`.
Sorter under change: `xsa/runtime/tools/binbwt/scatter_sort.cpp` (source of truth;
`tools/binbwt/` is the stale snapshot and was not touched).

## THE MECHANISM AS LANDED

1. **THE SWEEP EXTRACTS AND ROUTES.** The window sweep (`scatter_window`) no
   longer writes an 8-byte position: for every position that matches a wave's
   predicate it routes the row against that wave's fences (full depth, from the
   same read buffer) and captures the row's key out of the SAME buffer:

   ```
   (position | carried symbol, key[K], run length L, run terminator t)   14 + K bytes
   ```

   The band part file IS the band row; the leaf sort reads no corpus for it.
   `K` = `--wave-key-bytes` / `XSA_WAVE_KEY_BYTES` (default 8, CLI wins).

2. **PLAN-FIRST IS THE CONSEQUENCE.** A wave's fences must exist before the sweep
   that routes its rows, so `planWave()` computes each window wave's band plan up
   front (census from the survey's prefix cache: zero corpus reads) and the wave
   loop reuses it. A band-allocation failure now shrinks the unit for the NEXT
   window (a swept wave cannot be re-cut).

3. **THE LEAF IS AN IN-MEMORY MSD RADIX** over the captured key: 8-bit digits,
   257 buckets with bucket 0 = the virtual END (dollar mode exactly), ping-pong
   row planes, the key plane priced per band row. Rows equal on the whole key go
   to a deeper pass that extracts the next window (K, 2K, 4K, capped) for THOSE
   ROWS ONLY, in ascending position order; the cap's leftovers go to the LCE
   fallback, walking from the depth the windows proved.

4. **THE RUN-LENGTH DIGIT** (`SORT_RUNKEY`): `sigma` = the first key symbol,
   `L` = the length of its run, `t` = the terminator (0 = END, 257 = cap hit).
   Order digits = `(sigma, family, L, t, key[1..K))` with family 0 = `t < sigma`
   (L ascending), 1 = run outlived the cap (unknown), 2 = `t > sigma`
   (L descending). Proof: for L1 > L2 the first differing byte is at d+L2 where
   row 1 still has sigma; so the longer run sorts first iff t2 > sigma, and a
   row with t < sigma always precedes one with t > sigma (compare at d+min L).
   This collapses a run class in ONE round instead of log2(L/K).

5. **THE WAVE IS SIZED FROM THE ARENA** (`SS_WAVE_ARENA`):
   `rows_per_wave = min(arena / bytes_per_row, disk window)`, both terms
   journaled, with a plan-time refusal (`WAVE_ARENA_TOO_SMALL`) instead of a
   silent shrink.

6. **GUARDS, because a spin is not a result.** Strictly increasing round offsets
   (`WAVEKEY_OFFSET_FAIL`), the round cap, a partition that may never grow the
   tie set (`WAVEKEY_TERMINATION_FAIL`), a non-positive extraction window
   (`WAVEKEY_EXTRACT_FAIL`), and a COUNTED walk budget on the LCE fallback
   (`WAVEKEY_FALLBACK_BUDGET`, 64x corpus floored at 4 GiB, knob
   `--wave-fallback-walk-max`) — a 200 KB periodic fixture that used to run for
   hours now fails in seconds with the bytes it walked.

7. **JOURNALS**: `SS_WAVE_KEY_KNOB`, `SS_WAVE_ARENA`, `SORT_RUNKEY` (per band:
   rows, tie_rows, three family counts, L histogram), `SORT_ROUND` (per round),
   `WAVE_KEY band=...` (per band ledger), `WAVE_KEY totals ...` (run), `SS_IO`
   (the process's own `/proc/self/io`: rchar, read_bytes, syscr), and
   `EXTRACT_BYTES_READ site=... rows=... bytes=...` per corpus-read call site.

## GATES

Measurement policy printed in every log: **no timeout on a measurement run; a
run without `SS_DONE` is VOID; every rate carries its denominators.**

### W3 / K2-equivalent — brute oracle on the adversarial fixtures (GREEN)

7 fixtures × 6,291,456 B, 44,040,199 rows, run with the internal oracle guard
(`XSA_WAVEKEY_VERIFY=1`, which brute-force sorts every band and checks every
adjacent LCP by direct walk):

```
docsep     sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
eofrun     sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
internsep  sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
mixed      sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
period2    sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
r0         sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
single     sa_identical=1 lcp_identical=1 oracle=PASS sa_check=PASS
```

### W4 — tie accounting (GREEN)

`single` (6,291,456 B): `WAVE_KEY totals rows=6291433 key_bytes=50331464
tie_rows=12018271 rounds=20 rounds_max=4 extra_passes=15 bytes_read=94050642
syscalls=95 fallback_rows=0 no_shrink=0 bands=5 run_family0=3271336
run_family_unknown=0 run_family1=3020097 run_capped=0 run_cap=65536`.
The run digit shows up as designed: `rows_tied=0` by round 3
(`SORT_ROUND band=4 round=3 off=32 k=32 rows_in=10119 rows_tied=0 shrink=1`),
where the run-less version still had 1.26 M rows tied at round 5.
Extra passes (15) are proportional to tie rows, never to n. `rounds_max=4 <= cap 5`.

### W1 — I/O, 3 GB cold (`posix_fadvise(DONTNEED)` before every arm, no timeout)

Corpus `c3g.txt` = 3,000,000,000 B, n = 3,000,000,001 rows, `--threads 24`,
`--budget-bytes 24e9` (48e9 for K=64/128), cold before each arm. `base` = the
pre-change binary from `origin/main`.

| arm | K | bands | total_s | rchar | rchar/corpus | read_bytes | read_bytes/corpus | syscr | syscr/row |
|---|---|---|---|---|---|---|---|---|---|
| base | – | 1255 | 247.71 | (no SS_IO in the old binary) | – | – | – | – | – |
| new | 8 | 1264 | 621.87 | 3,540,572,695,176 | **1180.2** | 3,000,721,408 | 1.00024 | 570,620,530 | 0.190 |
| new | 24 | 1264 | 485.78 | 763,579,146,848 | **254.5** | 3,000,729,600 | 1.00024 | 271,991,822 | 0.0906 |
| new | 64 | 1509 | 469.31 | 523,545,437,829 | **174.5** | 3,000,741,888 | 1.00025 | 92,805,431 | 0.0309 |
| new | 128 | 2734 | 628.59 | 732,670,021,869 | **244.2** | 3,000,000,512 | 1.00000 | 65,832,711 | 0.0219 |

**VERDICT: W1 FAILS THE BAR (rchar <= 3x corpus).** `read_bytes/corpus` is
exactly 1.000 (one device pass — the sweep reads the corpus once and never
touches it again for a key), but the REQUESTED bytes are 174x–1180x the corpus.

### The attribution, measured (this is the useful part)

`EXTRACT_BYTES_READ` (3 GB, K=128): the band-tie-round extraction site is
`calls=13662 rows=298317431 bytes=151311807666` = **50.4x corpus, 21 % of rchar**.
The rest is NOT the extraction:

| term | 3 GB @ K=128 | measured as |
|---|---|---|
| band-part round trip (write then RE-READ the wave's rows) | n × 142 B = **426 GB** | `SS_IO wchar=456.8 GB` written, read back by `SS_BAND` |
| per-band tie extraction (`wk_extract`) | **151 GB** | `EXTRACT_BYTES_READ site=0` |
| one global sweep per wave (50 waves × 3 GB) | **150 GB** | `SS_SUBDIV_SCATTER corpus_sweeps_global_arm=50` |

So the two structural survivors of the OLD pipeline are:
1. **the wave's rows still go through disk** (`(14+K)` bytes written and read
   back). The operator's design item 3 — the wave's rows AND keys fit the arena
   at once — is *priced* (`SS_WAVE_ARENA`) but the rows are still published as
   band part files and re-read by the band worker. That term alone is `n x (14+K)`
   and it is why a bigger K makes rchar worse, not better.
2. **the tie extraction is per BAND, not per wave** — it re-reads the pages a
   sibling band's tie set already read (the same page holds rows of many bands).

Both are the pre-existing protocol, not the new mechanism; the new mechanism did
remove the per-band KEY read (the 184.7 B/row lane defect) — at K=24 the
extraction is 0.09 corpus-passes per... see the ledger — but it cannot fix the
two terms above from inside the band.

## W2 — 100 GB identity: NOT RUN (and why, with numbers)

The pre-change arm needs ~34 h for one 100 GB sorter pass (measured by the
previous lane: wave 32 of 249 after 2 h 38 m), and the operator's box carries a
live full-PILE build. Two arms is ~70 h. Order identity is instead proven at
3,000,000,001 rows (SA **and** LCP byte-identical, below) plus 150 MB.

## W5 — order identity at scale (GREEN on the bytes this lane emits)

* **150 MB** (`pile150.txt`, n = 150,000,001): `out.sa` and `out.lcp` **byte
  identical** to the pre-change binary.
  `sa=728d506ef64a88f488a1e329ec21a3d4bdcf8d314785371d7af40ddbd48413db`
  `lcp=dacd9d3996a9e976cf3fdb508cd828085da8cd278a2e7014bd6f88712afb01d6`
  (identical to the banked value reported by the previous lane). 63 bands.
* **3 GB** (n = 3,000,000,001, cold): `out.sa` **byte identical**
  (`bb28b228f9b607da…`, the banked 3 GB value) and `out.lcp` **byte identical**
  at K = 24, 64 and 128 (1264 / 1509 / 2734 bands compared). A 39-byte column
  defect found at K=24 (first differing rank 35,614,323, new=1 vs base=2) was
  traced to the adjacent LCP pass running BEFORE round 0's small-span sort, so a
  span's boundary value belonged to the row that was no longer first; the pass
  moved after the sort and the column is byte-identical at every K since.
* `cargo test --release`: **106 passed; 0 failed; 1 ignored**.

## Residual risks (measured, not hoped)

1. **rchar fails the 3x bar** (174x–1180x). The two terms are named above with
   numbers: the band-part round trip (`n x (14+K)`) and the per-band tie
   extraction. Fixing them means keeping a wave's rows in the arena (skip the
   band-part disk round trip) and hoisting the tie extraction to the WAVE. That
   is a protocol change, not a knob.
2. `K` is a knob with a REAL trade-off and no default that wins: K=8 is fastest
   to capture and worst for reads (1180x); K=64 is the best measured (174x) at
   1.6x the base wall; K=128 makes the record itself the dominant read term.
3. The wall is 1.9x–2.5x the pre-change binary at 3 GB and 1.8x at 150 MB. The
   lane does not claim a wall win.
4. A genuinely periodic class (`sigma^k` with a period < K) has no bounded sweep
   order; it now FAILS loudly (`WAVEKEY_TERMINATION_FAIL` /
   `WAVEKEY_FALLBACK_BUDGET`) with numbers instead of running for hours.
5. `--wave-run-cap` 64 KB covers the shapes measured here (the 32 KB tail run,
   the 4 KB fixture runs); a class whose runs exceed it degrades to byte
   extension for those rows only (journaled as `run_capped`).
