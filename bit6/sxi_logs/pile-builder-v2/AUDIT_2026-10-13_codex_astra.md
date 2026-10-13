# Sole-agent audit: what the pile pipeline actually requires, and what is possible
Agent: sxgc.codex-astra-writer (codex CLI, gpt-6-astra, reasoning high), sole agent, read-only charter.
Checkout audited: 979e36c8 (worktree clean; no repo modifications, no corpus access, no builds).
Citations: C=bit6/chunk_frontend.cpp M=bit6/cross_lcp_merge.cpp F=bit6/pile_finish.cpp
L=bit6/slim_lce.hpp H=xsa/src/chain.rs P=xsa/src/plan.rs R=pile-builder-v2/README.md
D=PILE_PLAN_V2.md E=REMAP_ERA_FINDING.md

## Verdict

The owner is right about storage sufficiency, but the proposed periodic-tie-only repair is wrong.
Large intermediates are implementation choices, not requirements of cyclic sorting or the lite product.
MORE SERIOUSLY: the finish is not bounded to 100 GB - it reconstructs all witnesses in RAM and
buffers entire encoded members. The current source cannot support an attested pile launch merely
by finding more scratch disk.

## Per-phase: actual vs intrinsic vs lean

| Phase | Actual | Intrinsic | Lean |
|---|---|---|---|
| Chunk | text n + doubled 2n + SA(2n)i32 8n + PLCP(2n)i32 8n + order 4n + runs 32x r + .pos 4n => 27n empirical heuristic, NOT a safe bound (C:170-244) | text + cyclic order + deterministic ties; runs can stream (C:178-203) | conservative doubled-sort in-place 10-11n; ambitious single-text 5n + workspace |
| Merge | m2.bin = T||T (2n) M:2299-2318, hs.bin ~2n, side BWTs n, packed WM 5.125n@w41 (=6.70 TB), overlays 1.45n => 11.575n = 13.77 TiB; whole-tree 16.415n = 19.53 TiB (P:466-480) | global text access + ordering state; k-way needs fronts+comparisons, not another text copy | corpus-backed comparator, bounded caches, streamed rows, seam metadata |
| Finish | PFCK + all witnesses reconstituted in RAM (F:1040-1099; chi/n=0.283 -> ~2.96 TB uint64 at pile), whole EF streams in BitsOut::mem (m5 ~0.196n = 256 GB; m1 reserves ~722 GB, F:1660-1690,1763-1765), corpus mmap of entire 1.31 TB range breaks under RLIMIT_AS -> silent 64KiB-page fallback (F:140-176; H:576-605) | exact LCEs + sequential witness state + final member bytes | bounded external witness merge, file-backed bit writers, windowed mmap, persistent workers |
| Product | 6 members, m4 empty (F:1658-1714) | those bytes + conventions | preserve serialization, replace scratch lifetimes |

## Q1-Q4 chunker / the owner's question

- Storage sufficiency PROVEN constructively: int32 positions + heapsort with comparator T[(p+d)%n],
  stop after n equal chars, tie by position => text + pointers + O(1) state (worst-case slow, but
  sufficient). The owner's intuition is correct.
- 27x decomposition: text n, doubled 2n, SA 8n, PLCP 8n, order 4n = 23n before runs (12.16n at
  r=0.38n) and .pos 4n+ slack. Empirical sizing heuristic, not an allocation bound.
- "Linear order differs only for periodic ties" is FALSE. Nonperiodic counterexample:
  T=[0x1e,0x0a,0x1e]: linear [1,2,0] vs cyclic [1,0,2]; suffix [0x1e] is a prefix of
  [0x1e,0x0a,0x1e] and wraparound reverses them. Valid theorem is NARROWER: a pair's order can
  change only when one finite suffix is a prefix of the other. Exact single-SA repair = identify
  every terminal-suffix prefix interval, keep maximal intervals, re-sort their contents
  cyclically. Nested intervals contained in maximal ones. Not implemented/certified here.
- PLCP has exactly ONE use: >=n common-byte grouping for tie repair (C:183-195). NOTHING PLCP is
  serialized; the finish computes its own LCEs (F:1544-1547). EXACT replacement: minimal cyclic
  period p, already computed for .pos (C:91-105). p=n -> no repair at all; p<n -> equal-rotation
  classes are residue classes mod p, emit ascending. Removes 8n + the PLCP pass, byte-identical
  outputs. Conservative front end => 10-11n; single-text could reach ~5n but repair may dominate.
- libsais has no wraparound comparator API (token-libsais.h:75-84,172-182); single-text needs a
  separate repair algorithm.
- Hard bound n <= 1,073,741,823 (C:168) => pile needs >= 1219 chunks; at ~1 GB/chunk and 27n the
  front door budgets only 55% of cap => ~2 workers at 100 GB (H:862-867).

## Q5 merge transient

- m2.bin is an explicit T||T copy (M:2299-2318) - the plan's "no text materialization" claim is
  FALSE at the merge (D:35-40). The finish IS corpus-backed; conflating the two hid the copy.
- 10.20n = m2 2n + hs ~2n + side BWTs n + WM w.n/8 + overlay residual ~1.45n (P:459-480).
  At w=41: 11.575n = 13.77 TiB live at the final merge; whole-tree 16.415n = 19.53 TiB.
- WM = merged SA position sequence, NOT just a wavelet matrix (M:2542-2548; F:627-640). Three
  consumers: run counting, record emission, seam-class row/flank reads (M:2877-3021; F:627-692).
- Early-deletion possible after core merge (hashes/BWTs/overlays dead before emission,
  M:2292-2296,3558-3565) but does NOT remove the walk peak.
- Proposed bounded shape: pre-discover seam targets, capture rows/flanks while streaming, spool
  class records; emit shard-local run streams, stitch boundaries, assign global bases. Removes
  whole-WM requirement; changes the index/piece protocol; must be re-gated.
- Virtual T||T via modulo coordinates + remap removes 2n. Sparser checkpoints (CROSS_HASH_K)
  trades reconstruction for disk. Piece window ~64 MB at w=41 (M:2753-2773) - the planner's
  0.052n is a 1 GB observation wrongly linearized (P:455,531).

## Q6 accretion / errors

- v2 route leaves the endpoint/slim/aggregate/sweep branch live below it (H:885-940) - dead for
  v2, live for v1. Legacy SXCR/SXP modes selectable but unneeded for the one canonical route.
- Finish decodes the whole rlebwt for a completion test, then again for m1 - sentinel can replace
  the first scan (F:1768-1836).
- Duplicate seam evidence: corpus occurrence set vs WM class set sorted+compared, then re-sorted
  - gate/debug only, not product data (F:683-701).
- LCE fingerprints propose, then every byte rechecked - that IS the exactness guarantee; keep
  unless replaced by a proven exact mechanism (L:312-344).
- FALSE claims found in committed docs: "no text materialization" (m2 is 2n of text on disk);
  "bounded witness finish" (all spills reconstituted).
- Alphabet migration incomplete: v2 widening of 0/5 does not establish v1 parity or all sentinel
  conventions (F:816; E:43-91).
- NO bijection on all 256 bytes can vacate six values; order-preserving bijection on the full
  ordered alphabet is identity. A +6-style remedy requires restricted alphabet or changed
  representation, not another permutation (E:61-69; sxi_format.hpp:39-44).

## Q7 ranked levers (all proposals, none implemented/benchmarked)

| # | Lever | Gain | Risk |
|---|---|---|---|
| 1 | Stream witness merge + file-backed member encoders | removes TB-scale finish RAM | medium: witness order/padding/CRCs |
| 2 | Virtual corpus-backed m2; prompt scratch release | removes 2n, LAW restored | medium |
| 3 | Eliminate whole WM (run streaming + seam capture) | removes 5.125n = 6.70 TB | high: shard/seam protocol |
| 4 | Drop chunk PLCP/order/run duplication (period grouping) | 27n -> 10-11n, more workers | low-medium, exact |
| 5 | Resource accounting: bounded mmap/cache, shared envelope | feasibility, no silent fallback | medium |
| 6 | Single-text SA + full prefix-interval cyclic repair | toward 5n | high |
| 7 | Persistent LCE workers, completion sentinel | unmeasured | low-medium |

Planning-experiment targets (NOT forecasts): chunk 30-80, merge 120-250, finish 120-220,
witness/encode 30-70 s/GB => 300-620 s/GB => 4.5-9.4 days at pile, no overlap credit.
Scratch target after levers 2+3: ~3.5-5n = 4.6-6.5 TB (fits the box; kills the 13-20 TiB
hardware blocker). Re-gate: full container byte-compare at 100 MB/1 GB, exact witnesses/chi,
nonperiodic wrap-prefix cases, periodic chunks, nested prefix intervals, 0/5 + remap-active
bytes, forced spills, forced mmap fallback; then a cache-cold measured rung with live-file
accounting.

## Q8 wall model

- 1,307.91 GB: 500 s/GB = 7.57 days; 632 s/GB = 9.57 days; ~793 s/GB = 12 days. The checked-in
  planner constants predate all six milestones (3605.5 s/GB -> 54.6 days, P:495-498) and need
  reconciliation; the recorded post-milestone number is 737 s (contaminated), and 632 is not in
  the committed narrative (R:599-609).
- The 128n seam limit is a WORK-REFUSAL budget, not a time coefficient; recorded main-LCE work
  was ~17.5n (R:614-618).
- Omitted scale penalties: log k heap comparisons at 1219-way, ~3.8 GiB front windows, cache
  coverage, forced mmap fallback, per-band 96-worker respawn (F:1531-1562).
- FIRST-ORDER CONCLUSION: infeasibility before timing - scratch, witness RAM, encoded-member
  RAM and address-space behavior must be repaired FIRST; exact LCE/comparison I/O is the
  principal scaling uncertainty after that.
