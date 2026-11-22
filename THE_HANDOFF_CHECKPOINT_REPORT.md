# CHECKPOINT REPORT — Amendment R, Rust port of the TAPE_READER machine

## (a) Commit at pause
**`50c84c4`** ("R: fix the pieces start header — width is u64, not u32"), branch `feat/tape-reader`, pushed, `origin == HEAD` verified (uniq count 1). **Worktree clean** — `git status --porcelain` count 0. Commits this era: `9f553d1` (W5 gate) → `9b41d0b` (the unit: pairsbins B/B4 + emit + assembly; G0 9/9) → `8f332e8` (word-chunk law + cost-naming receipts) → `50c84c4` (start-header width fix).

## (b) Running jobs at pause
**None.** The 49-min refuse arm was reaped by the orchestrator; the wide sandbox arm completed on its own (producer rc=0, finish rc=2); `pgrep` count 0.

## (c) The LAST PASSING command
```
env TRIE_TAP=1 SS_LCP_SLICE_WINDOW=1048576 CROSS_V2_PIECE_WINDOW=1000000 \
  /home/erikg/wt/tapereader/xsa/target/release/pfp_sort_r \
  --corpus /tmp/tr_p3/fixtures/mixed.txt \
  --out-prefix RUST_mixed_w2/merged/frag --bands RUST_mixed_w2/bands \
  --budget-bytes 16000000000 --threads 8 --scratch-budget-bytes 301989888
```
rc=0; band SHA `31a764b6d6787d5e8d572818cc19d8ac715507e7623056e307df564345644eb3` — unchanged by the word-chunk law and **identical to the C++ oracle's certified G0 band-invariance record**. Receipts: `SS_EMIT_UNIT unit=1 rows=448236 window=0 lcp_fast=71 lcp_bounded=185190 lcp_group=19623 lcp_residual=243350`; `SS_DONE_R rows_emitted=6291456 end_rows=1`. Behind it: the full G0 suite through the Rust machine — 8/8 fixtures brute-ORACLE PASS (adv docsep eofrun internsep mixed period2 r0 single), band invariance at **three** dials (wide 11 bands / 16MB 99 bands / 64MB 27 bands) all → `31a764b6…44eb3`.

## (d) The NEXT failing-or-untested command
```
/tmp/tr_smoke/run_dial_r.sh wide wide
```
(the harness mirrors `run_dial_b.sh`: `pfp_sort_r --corpus sandbox/pile-slice100.bin --remap remap-256.bin --threads 24 --scratch-budget-bytes 200000000`, finish `/tmp/xsa_finish_cur` concurrent). The arm's prior verdict: producer **rc=0, SS_DONE_R rows_emitted=100000503 end_rows=1, SS_SCRATCH_PEAK peak_live_bytes=190,139,749 ≤ 200,000,000** (the strict 2×N law held) — but the finish died `FATAL xsa_finish: stream start size`: my pieces' `start` header was 32 bytes (width written as u32). **Fixed at `50c84c4`; the fix is UNTESTED at sandbox scale** — that re-run is the next gate, expecting `CONTAINER_FINAL bytes=107519008 sha256=6325c843c0d9…f341`. Then the 162MB and 5122MB dial arms, then 1 GB (`b20ceebe…`), then 3 GB member identity, then the 100 GB package.

## (e) The refuse verdict (word-chunk law, 8f332e8)
**Neither pass nor die — the cost is now named and it is honest, not a loop.** Receipts: the L1 landing walk is linear (`SS_L1_PROGRESS groups=55503/65631 rows=5319072 elapsed_s=60.0`, ~71s to complete 65,631 groups / ~6.3M rows; `BIN_LOOP_STALL` never fired). The real cost is the **residual LCP class** in the assembly: on the 5-byte-period corpus, most adjacent SA rows have one slice a strict prefix of the next → MB-long `suf_lcp` walks. The C++ oracle pays the same class — its own arm ground 48+ min unfinished (its phase was silent; no C++ receipts exist for it). The word-chunked Rust arm ran to ~44 min of my polling (reaped at ~49) still in the assembly — projecting ~45–60 min total, ≈ the C++'s cost; the byte-at-a-time shape I replaced would have taken 5–12 h. **Crucially: refuse is NOT a G0 member** — `gates/tape_bins_g0.sh` deliberately gates 8 fixtures + `mixed_64mb` only; no machine, C++ or Rust, has ever taken refuse through the bins path to completion. It is a bonus arm, off the ladder.

## (f) What the docs don't say
1. **The u64-literal arrival-record class**: Rust tuple literals like `push((0, p, o, oi))` infer u64 → 20-byte "16-byte" records. Two such writers bit me (window fwd flush, bin fwd flush). Every future arrival-record write must cast every field to u32.
2. **The L1 rank merge**: the C++ inner heap merges a group's occurrence lists *by continuation rank*; pair-by-pair concatenation transposed 873K adjacent rows on mixed (1.7M oracle mismatches, all adjacent swaps). Equal-slice groups order BY CONTINUATION RANK, never by pair.
3. **The finish's dir contract**: `merged/` and `bands/` must pre-exist, `frag.pieces` must be ABSENT ("must be fresh") — manual runs trip this; the harness scripts handle it.
4. **The seam I trust least**: the pieces' LCP-slice bytes (SXLS 255-escape format, `band_{u-window}.lcp.consumed` handshake) have never been successfully consumed by the finish — the one arm that tried failed on the (now-fixed) start header. Only the re-run sandbox arm exercises it end-to-end.
5. **Scratch peaks run ~10MB higher than the C++** (190.1 vs 180.2 MB at the sandbox dial) — within the law, but arrival-buffer accounting differs; don't expect identical peaks.
6. **Wall contamination**: the 288.74s wide-arm producer ran while the refuse arm ground a core on the same box; re-run arms on a quiet box for honest wall tables (C++ anchors: 145.07/71.20/65.58s).
7. **The level-2 path is module-gated only** (e178e70: m2=10849 at ceiling=100000, FFI libsais) — the full two-level product identity vs the C++ ASan receipt has not been re-run at fixture scale. The 100 GB census needs this path (M=3.05e9 > 2³¹, expected level-2 ~102M → 32-bit kernel, no libsais64). Gate it at sandbox scale first.
8. **Artefacts**: fixture runs under `TAPEREADER/bins-g0/RUST_*` (refuse log keeps the receipts), the failed-finish arm kept at `sandbox/br_wide` (finish.log has the FATAL), harness at `/tmp/tr_smoke/run_dial_r.sh`. C++ binaries for the ladder: `/tmp/pfp_b2` (current oracle — note `/tmp/pfp_b` is an OLD pre-B build whose CLI dies on this harness), finish `/tmp/xsa_finish_cur`, brute `/tmp/brute_sa`.