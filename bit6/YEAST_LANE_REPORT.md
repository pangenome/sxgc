# Yeast lane report — r-space χ/sA construction at 3.34 Gbp (GREEN)

Date: 2026-09-25. Worktree /tmp/sxgc-laneY, artifacts /tmp/laneY.
No git commits made (per protocol).

## THE GATE (Stage D)

```
xsa chi-rspace --ri4 /tmp/laneY/yp2new.ri4 --agg /tmp/laneY/yeast_pfp2.agg -o ...
xsa chi-rspace: chi = 85404240 (N=3336986761, R=100904881)
```
- **chi(yeast_pfp2 chain) = 85,404,240 == the gate value. EXACT.**
- **Stretch: witness SET EQUALITY vs the production oracle chi_yeast_pfp2.sA**
  (both 85,404,240 u64 values; numpy sorted-compare: `SET EQUALITY: True`).
- Rust sweep cost: **97 s wall, 6.0 GB RSS**, no text, no SA, no LF walks.

## The critical discovery: the staged inputs were cross-chain mispaired

The originally-staged pairing (y2new.ri4 + y2.lcp_index + parse yeast2_pfp
+ gate 85,404,240) mixed TWO different yeast chains:

- **y2new.ri4 + y2.lcp_index + chi_yeast.sA / chi_yeast_opt.sA (85,350,673)
  = the yeast235.rl.txt chain** (newline-joined revlines, 9901 strings,
  n = 3,336,986,759 = filesize exactly). This is the "chi(BCR)" row of the
  syng table.
- **The 85,404,240 gate = the yeast_pfp2.txt chain** ('!'-joined revlines
  single string, 9901 '!' separators, n = 3,336,986,760 = filesize):
  yeast_pfp2.rl_bwt -> yp2t.lcp_index -> oracle chi_yeast_pfp2.sA;
  the parse yeast2_pfp (PF.n = 3,336,986,770) is over THIS text.

Evidence: mtimes (y2.bwt.heads 09-19 22:27 right after yeast235.rl.txt
22:19; yp2t.lcp_index 09-20 14:24 right after yeast_pfp2.rl_bwt 14:22;
chi_yeast_pfp2.sA 23:53 from yp2t), n values, first-string comparison
(different lengths/contents), separator layouts (newline positions vs '!'
positions differ), and xsa stats.

The earlier "2.9% scattered BAD rows" in the flat spot-check was entirely
this mispairing: two different texts (same collection, different join) ->
two different suffix orders -> resolve right on ~97% of rows only because
the texts mostly agree. NOT a pfpds bug, NOT a multi-string issue, NOT the
trailing-newline seam.

### Discriminator outcomes (the requested probes, on the mispaired chain)

1. Round-trip / LF-composition (forward-only form: resolve(LF(r)) must
   equal pos-1; LF built from the .ri4 runs themselves): **ALL 235 bad rows
   failed** (lfOk=0/235) — resolve was genuinely wrong on bad rows, not a
   text-object skew of correct positions.
2. Row-direction probe (rows j-2..j+2, bwt/pos cross-comparisons): no
   neighbor-carrier structure; several bad rows had bwt absent from
   flat[pos-2..pos] entirely — pure position off-by-one excluded.
3. LF-consistency: same as 1 — decisive between "resolve wrong on r" vs
   "row-space mapping differs around r".
4. Shared structure of bad rows: none. NOT near string boundaries
   (distBackTo0A > 8 KB), NOT near the file end (distEnd scattered
   0.8-3.3 GB), bwt chars uniform ACGT, badRunStart/badInterior both ~3%.
   The orchestrator's trailing-newline seam hypothesis is REFUTED.
5. The saV4 samples of y2new.ri4 decode as fsFwd[i]+fstart[i]+len[i]-pos
   (reflected per-sidecar-string coordinates; sa_decode Test B: plus=2676,
   minus=0) — consistent with the revlines sidecar yeast235.rl.txt.names.tsv.

## The fix

Built the missing .ri4 for the pfp2 chain directly from the rlbwt pair
(yp2t.bwt.heads + yp2t.bwt.len): tools at /tmp/laneY/ri4_from_rle.cpp,
conventions copied verbatim from teralcp_chi build_runs_files
(n = sum(l), C[c] = cumulative-before, K = count of 0x0A symbols in the
BWT, sdsl int_vector header, samples = INF — unused by this pipeline).
**Byte-identical to the teralcp_chi-produced .ri4 on the battery** (header,
C, runs sections all equal). Output: /tmp/laneY/yp2new.ri4
(n = 3,336,986,760, k = 1, R = 100,904,881, sa_w = 32).

## The run (pfp2 chain)

- Dumper: /tmp/laneY/dump --ri4 yp2new.ri4 --parse yeast2_pfp
  --lcp-index yp2t.lcp_index.lcp_index --flat yeast_pfp2.txt -t 16
- Row-space calibration (hardened this session: full-probe offset scoring,
  unique zero-bad requirement): **off = W = 10 uniquely zero-bad** (off 8/9/
  11/12: 27/15/13/20 bad of 511); flat spot-check **512/512 ok, 0 bad**.
  Convention: machinery row m <-> SA-of-text row m-W, resolve_row(j) =
  SA_sup(j + W), no special cases (the parse covers the same bytes the
  rlbwt indexes, separators included).
- pfp n check now EXACT: PF.n = 3,336,986,770 = ri4.n + W (no warning).
- Aggregates: 100,904,881 runs; **LCE cross-checks = 1,190,377, mismatches
  = 0** (piece-law topLCP vs PFP LCE); ~10 min pfp machinery build +
  ~1.5 min aggregates at 16 threads; RSS ~29.7 GB.
- Resolutions: 2 per run (saFirst/saLast) = ~201.8M pfp_sa_support calls.
- Rust: chi-rspace consumes .ri4 + .agg only. 97 s, 6.0 GB RSS.

## Battery regression (final binaries)

All 8 texts GREEN (G1 aggregates == production --triples per-run, 0
mismatches; G2 chi + witness set == teralcp_chi oracle, seteq=YES):
random-4-2k 1307, random-4-20k 13423, satellite-18k 322, HOR-nested 214,
dup+unique-120k 48696, random-bin-20k 9653, random-4-200k 135968,
duplicates-600k 13004 (the historical blind-spot text: exact).

## Honest deviations from the staged plan

1. The Rust subcommand consumes C++-dumped aggregates (.agg) rather than
   parsing pieces/dict in Rust (pfpds+sdsl reuse made pure-Rust infeasible
   in scope). Pieces are consumed in the dumper (piece-law topLCP via
   PhiLookup, cross-checked against PFP LCE).
2. The sweep is the production scan-rs state machine ported verbatim into
   Rust (the production machine is the gated oracle), not a Python FM
   sweep port.
3. resolve() = pfpds::pfp_sa_support + the empirically pinned row-space
   convention above, calibrated per-text against the flat (unique zero-bad
   required, else FATAL).
4. New tooling: /tmp/laneY/ri4_from_rle.cpp (.ri4 from rlbwt pair, battery
   byte-checked), /tmp/laneY/sa_decode.cpp (saV4 convention decoding:
   Tests A/B), /tmp/laneY/battery_chain.sh (full per-text chain with G1/G2).

## Files

- /tmp/laneY/yp2new.ri4        (908 MB) runs-only .ri4, pfp2 chain
- /tmp/laneY/yeast_pfp2.agg    (3.2 GB) per-run aggregates
- /tmp/laneY/yeast_pfp2.rs.out (683 MB) 85,404,240 witness values (u64)
- /tmp/laneY/yeast_pfp2_dump.log, yeast_rust.log, battery_final.log
- Source: /tmp/sxgc-laneY/bit6/chi_rspace_dump.cpp (hardened calibration +
  diagnostics), xsa/src/main.rs (cmd_chi_rspace, unchanged this round)

## Open items for the parent

- 466 confirmations and the "which oracle is the headline" bookkeeping:
  BOTH yeast chains are real objects; chi(yeast235.rl) = 85,350,673 and
  chi(yeast_pfp2) = 85,404,240. The RESEARCH/BIT_LADDER record cites
  85,404,240 as "chi(yeast)" — it is the pfp2-text chi; the drift-law
  tables should say which text each number belongs to.
- syng cross-check (proc_3b41 still building at last check) uses yet
  another text object; same bookkeeping caution applies.
