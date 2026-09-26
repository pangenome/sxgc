# PFP-LCP PRIMITIVE PORT — GATE LOG + HONEST COST REPORT

Lane: PFP-LCP port. Worktree `/tmp/sxgc-laneI` at `67f7b47`.
Deliverable: `bit6/chi_rspace_dump.cpp` no longer needs `--lcp-index`
(or TeraLCP at all); the per-run aggregates `.agg` are produced from
PFP artifacts + `.ri4` only. Diff: `bit6/chi_rspace_dump_lce.patch`.

---

## THE CORE FINDING (why this was smaller than expected)

The dumper's *only* lcp_index consumption was `topLCP[i] =
lcp_at_pos(idx,P,pf)` — the **PLCP value at the run-head row** (pf =
`resolve_row(a)`). But PLCP at a row is, by definition,

    PLCP(SA[a]) = LCP(SA[a], SA[a-1]),

i.e. the classic **adjacent-row LCP** — the previous suffix in SA order
is row `a-1`, whose position is `resolve_row(a-1)`. So

    topLCP[i] = clamp( LCE_sup( resolve_row(a-1), resolve_row(a) ), n - max(·,·) )

with `a == 0 ⇒ topLCP = 0`. This uses **only** the vendored `pfpds::
pfp_lce_support` — which is exactly the standard PFP-LCP primitive the
task named: dictionary RMQ (`rmq_lcp_D`) + parse ISA/RMQ (`rmq_lcp_P`)
+ rank/select, **polylog per call, with no phrase-walk loop**. (The
O(LCP-value) phrase walk seen by the M3 lane was the Python prototype;
the vendored C++ support had no such loop.)

Therefore **no new primitive had to be written**: the machinery already
held it, and the lcp_index was simply redundant for this consumer.

`interiorMin` and both resolve directions were already PFP-only; the
port touches nothing else.

---

## GATE LADDER

Gate set = the 7 battery texts of `bit6/chi_rspace_battery_chain.sh`
(including **duplicates-600k**, which has caught two false greens) plus
the real yeast chain. Per-text artifacts in `/tmp/laneY/bat/`
(`.ri4`, `_pfp.parse/.dict`, `t.lcp_index.lcp_index`) and the committed
yeast artifacts (`yp2new.ri4`, `yeast2_pfp`, `yeast_pfp2.agg`,
`chi_yeast_pfp2.sA`).

### G0 — primitive vs lcp_index ground truth, at ALL positions

New env mode `G0_ALL` validates the identity at **every row** (rows
cover every position exactly once), i.e. for all m in [1,n):
`lcp_at_pos(idx,P,SA[m]) == clamp(LCE_sup(SA[m-1], SA[m]))`.

| text | rows checked | mismatches |
|---|---|---|
| random-4-20k | 20,000 | 0 |
| random-bin-20k | 20,000 | 0 |
| random-4-200k | 200,000 | 0 |
| satellite-18k | 18,500 | 0 |
| HOR-nested | 18,360 | 0 |
| duplicates-600k | 600,000 | 0 |
| dup+unique-120k | 120,000 | 0 |

**997k positions, 0 mismatches.** (Run with `G0_ALL=1` + `--lcp-index`.)

### G1 — aggregates, 7/7 byte-identical, NO lcp_index input

`dump_final --ri4 F.ri4 --parse F_pfp -o out.agg` (no `--lcp-index`)
vs the committed `.agg` from the old lcp_index path:

| text | `.agg` byte-identical |
|---|---|
| random-4-20k | YES |
| random-bin-20k | YES |
| random-4-200k | YES |
| satellite-18k | YES |
| HOR-nested | YES |
| duplicates-600k | YES |
| dup+unique-120k | YES |

### G2 — yeast, full scale, NO lcp_index anywhere

    dump_final --ri4 /tmp/laneY/yp2new.ri4 \
               --parse /mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast2_pfp \
               -o yeast_final.agg \
               --flat /mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast_pfp2.txt -t 32

- calibration: `ROW_OFF=10` (zero-bad offsets: **1** — uniquely pinned);
  flat spot-check `ok=512 bad=0`; LCE cross-checks 1,190,377 mismatches 0.
- `yeast_final.agg` **BYTE-IDENTICAL** to the committed
  `yeast_pfp2.agg` (3,228,956,204 bytes; R = 100,904,881).
- `xsa chi-rspace --ri4 yp2new.ri4 --agg yeast_final.agg`:
  **chi = 85,404,240**, witness **sorted-set equality vs the production
  oracle `chi_yeast_pfp2.sA` = True** (93.6 s / 6.0 GB).
  (Re-run with the final deliverable binary `dump_final`: yeast FINAL
  `.agg` 841.60 s / 25.3 GB, byte-identical; same chi GREEN.)

The standing yeast gate now holds with **no lcp_index input anywhere**.

### G3 — cost

| build | wall | maxRSS |
|---|---|---|
| baseline (with lcp_index) | 901.03 s | 28.4 GB (29,728,600 KB) |
| port (no lcp_index), run 1 | 867.70 s | 25.3 GB (26,518,196 KB) |
| **port `dump_final` (no lcp_index), run 2** | **841.60 s** | **25.3 GB (26,520,272 KB)** |

Net: **−33 s wall, −3.2 GB RSS.** Removing the lcp_index removes the
2.3 GB file read, the per-run binary search over the Phi pieces, and —
pipeline-wide — the **TeraLCP O(n)-time construction** (M1: TeraLCP is
O(n) time, O(r) space) is no longer needed at all.

#### Honest phase breakdown at yeast (port run, `-t 32`)

| phase | approx |
|---|---|
| dict + parse construction (pfp_ds) | ~4.0 min |
| pf_parsing: b_p, **b_bwt and M**, W-wavelet | ~4.8 min |
| calibration + flat spot-check | ~1 min |
| per-run arbitrary-length queries | ~5 min |
| **total** | **867.7 s** |

Residual Ω(n)-scaled work that REMAINS: `pfpds::pf_parsing::
build_b_bwt_and_M()` allocates and zeroes an **O(n)-bit** `b_bwt` (its
comment: "bug in resize") and materialises `M` with **|M| = 317,476,865
≈ 0.1n** entries; it also builds the dictionary/parse/RMQ structures.
This is PFP-index *construction* (one-off, alongside the parse), not a
per-query text scan, but it does scale with n and it dominates the
construction half of the wall time. The **per-run query phase is
O(r·polylog)** (2 resolves + ≤2 LCE per run; LCE = rank/select + two
RMQs).

**So:** the lcp_index is dead and TeraLCP is off the critical path; the
pipeline's remaining Ω(n) is the pfp++/pfp_ds index construction (the
dictionary+parse+pf_parsing build, of which the single pfp++ text read
is one part). Fully eliminating an O(n)-scaled *construction* inside
the dump would require reusing a prebuilt `M`/`b_bwt` (pfp++ does not
currently persist them) — see PORT_HANDOFF.md.

---

## TRAPS HIT / NOTES

- **Artifact mispairing (recorded).** ft30 / sat4 fixtures in
  `/tmp/grl_gate` were tested by regenerating pfp parses from
  `ft30.txt`/`sat4.fa`; the regenerated parse does not match those
  fixtures' `.ri4` text (different chain), giving spurious `idx=0` G0
  lines and even a baseline core dump. **Not counterexamples** — the
  same cross-chain mispairing class the yeast lane hit. Those fixtures
  need their own chain rebuild; excluded from the gate set.
- The `--lcp-index` argument is retained as an **optional cross-check**
  only (`CHECK_TOP` env, or `G0_ALL`); normal operation needs neither it
  nor `--flat` (with no `--flat`, `ROW_OFF` defaults to `W=10` and no
  calibration runs).
- Build line (pinned): `g++ -O2 -std=c++17 -DM64=1` with
  `pfp_ds-src/include`, `spdlog-src/include`, `gsacak-src`,
  `TeraTools/src/thirdparty/include`, sdsl-lite includes, linking
  `/tmp/laneY/gsacak64.o` + sdsl.
