# RESULTS — no match may cross a document boundary (the no-cross lane)

**The requirement:** the owner's semantic law, as the DEFAULT, not an option:
for an occurrence of a context at `pos` (length L), the continuation at
`q = pos + L` is the **END branch** iff `q == text_len` or
`select(rank(q)) == q` (`q` is a document start — the exact predicate
implemented, one m15 rank/select per classification), and its **content byte**
`corpus[q]` otherwise; the occurrence is a **crossing match** iff
`rank(pos) != rank(q-1)` and is then counted nowhere. For search, a match
`[s,e)` crosses iff `rank(s) != rank(e-1)` and is excluded from the default
output. Supervisor decision (B): marked fallback, never fatal, raised cap,
strict mode — the refusals live on the trie's own query surface.

**Branch:** `no-cross` (this lane, merged to main). All gates on this box
(2×AMD EPYC 7713, 256 threads, 1007 GiB RAM), foreign load throughout (the
10 GB build ran beside the X1 fixture gate and the X4 search arm: load 30–44
during the consult/search arms, 8–39 during the build).

**Data:** `/mnt/nvme2n1/erikg/pf-10gb.txt` (10,000,000,000 B), document map
`/tmp/map10g` (XSEP v1, 1,606,585 documents, 446 internal 0x1E — the banked
10 GB prefix map, RESULTS_PILE_DOCMAP.md).

---

## X1 — adversarial fixtures, brute-force oracle (PASS, run twice)

`gates/structdocs_fixtures.py` (the G2/G3 gate, extended with the X1 arm);
oracle `brute_law` = memmem over the fixture + the starts table, no index, no
shared code. Both runs exited 0; verbatim from the final run:

```
X1_HISTOGRAM x1a arm=LAW width=5 eof=3 pairs=[(81, 2)]
X1_HISTOGRAM x1a arm=CONFLATED_legacy width=5 eof=0 pairs=[(81, 2), (85, 2), (116, 1)]
X1_NO_LEAK x1a doc_ending_occurrences=3 next_doc_bytes=[85, 116] absent_from_content=1
X1_TRIE_BUILD x1a aware=1 source=member-m15 docs=6
X1_TRIE_BRANCH_OF x1a count=5 end=3 pairs=[(81, 2)] oracle=width=5 eof=3 pairs=[(81, 2)] exact
X1_TRIE_VERIFY x1a TRIE_VERIFY n=200 exact=200 mismatches=0 below_min_support=0
X1_UNAWARE_REFUSAL x1a rc=1 refused=yes
X1_UNAWARE_CROSS_DOC x1a count=5 end=0 pairs=[(81, 2), (85, 2), (116, 1)] parity=conflated_exact
X1_HISTOGRAM x1b arm=LAW width=3 eof=1 pairs=[(30, 2)]
X1_HISTOGRAM x1b arm=CONFLATED_legacy width=3 eof=0 pairs=[(30, 2), (85, 1)]
X1_NO_LEAK x1b doc_ending_occurrences=1 next_doc_bytes=[85] absent_from_content=1
X1_TRIE_BUILD x1b aware=1 source=member-m15 docs=4
X1_TRIE_BRANCH_OF x1b count=3 end=1 pairs=[(30, 2)] oracle=width=3 eof=1 pairs=[(30, 2)] exact
X1_TRIE_VERIFY x1b TRIE_VERIFY n=200 exact=200 mismatches=0 below_min_support=0
X1_UNAWARE_REFUSAL x1b rc=1 refused=yes
X1_UNAWARE_CROSS_DOC x1b count=3 end=0 pairs=[(30, 2), (85, 1)] parity=conflated_exact
X1_RESULT PASS
G3_EXCLUSION positions=120 width_on_equals_width_off_minus_1=120 both_directions=1 result=PASS
G2_G3_RESULT PASS
X1_FINAL x1_failures=0 all_failures=0 result=PASS
```

x1a is the law's headline: FOUR contexts' occurrences end documents — the
histogram's END branch carries exactly 3 (the doc-ending occurrences), the
content branch carries only the two in-document `Q` continuations (81), and
the next documents' first bytes (85 'U', 116 't') appear in NO content branch.
The conflated legacy answer leaks them — and that difference, `eof 0 -> 3`,
is what the row format now reports. x1b is the distinction the owner named:
the internal `0x1E` continuations stay a CONTENT branch (30, ×2) while the
doc-ending occurrence moves to END — the conflated answer counted the next
document's first byte (85) as content instead. The boundary-aware trie
(m20, member format v3, `boundary_aware=1`, built post-hoc from the
container's own m15) answers `branch-of`/`trie-stat` with the same exact
histogram, `ctx-verify` (corpus-scan oracle) passes 200/200 on both fixtures,
and the UNAWARE trie refuses by default and answers the conflated histogram
only under `--cross-doc` — matched against the legacy oracle to the byte.

Unit scale, before all of this: `xsa ctx-selftest` (the builder vs the
boundary-aware brute-force trie oracle, metadata starts placed mid-pattern):
`CTX_SELFTEST ... contexts=24407 cut_contexts=101796 cut_nodes=103188`;
`xsa ctx-tap-selftest` (band stream == suffix array, byte-identical
boundary-aware members, END counts spot-checked):
`TAP_SELFTEST_DONE cases=24 cut_cases=5 ...`.

## X2 — the combined 10 GB build (PASS)

```
X2_BUILD_START 2026-11-11T14:49:26Z flags=--tape,--trie-depth-8,--trie-min-support-1000,--docs-map=/tmp/map10g,--scratch-budget-gb-3,--chi-expect-2676925006
X2_BUILD_END rc=0 2026-11-11T15:34:29Z
X2_WALL 2702.89 s peak_rss_kb 12293012
XSA_BUILD_DONE chi=2676925006 pipeline=v2tape container=/mnt/nvme2n1/erikg/no-cross/x2/scratch/pf-10gb.sxi2
X2_CONTAINER path=/mnt/nvme2n1/erikg/no-cross/x2/scratch/pf-10gb.sxi2 bytes=7383791080 version=10 n=10000000001 flags=825
X2_MEMBERS m1(codec116,3254868712B) m4(codec4,12B) m5(codec105,1294231276B) m7(codec7,256B) m10(codec110,0B) m11(codec111,27147552B) m12(codec112,27571784B) m13(codec115,2400727096B) m15(codec15,3604040B) m20(codec20,375639880B)
X2_M20 member_version=3 boundary_aware=1 depth=8 min_support=1000 nodes=3591326 edges=88031509
X2_M15 bytes=3604040
X2_CHECK members=ok m14=absent m15=present m20=present boundary_aware=1
X2_SHA256 ecb2e766af6984c5f5b898d8c8f889af7b1cb19b8685365f2a75b5081469e07d
```

- **Container:** 7,383,791,080 B (6.88 GiB) over the 10 GB text, SXI2 v10,
  members m1,m4,m5,m7,m10,m11,m12,m13,**m15**,**m20** — **m14 absent** by
  default (it is opt-in), chi **2,676,925,006 unchanged** (asserted by the
  build itself).
- **The boundary-aware trie, in one build with everything else:**
  `CONTEXT_TRIE_BOUNDARY aware=1 doc_starts=/tmp/map10g docs_in_table=1606585`,
  member format v3, 3,591,326 nodes / 88,031,509 edges = 375,639,880 B =
  **0.037564 n** (target ≤ 0.05). The tap fed all 10,000,000,001 rows:
  `CONTEXT_TRIE_TAP ... wall=2567.5s work_s=1381.0s ... rss_gb=9.709
  threads=48` — the tap's own work includes the per-row `avail` rank/select
  and the per-row cut classification.
- **m15:** 3,604,040 B = 0.00036 n; `DOCSTARTS docs=1606585 ... gaps_vs_plain=0.2570
  text_len=10000000000 map_corpus_bytes=10000000000 in_place=1`.
- **Peak scratch vs the 3 GB budget:** `SCRATCH_BACKPRESSURE_SUMMARY
  budget_gb=3.000 peak_gb=8.622 floor_gb=8.615 pauses=3 resumes=3
  paused_ms=69273 ... accounting=scratch_excludes_container` — the budget's
  pause mechanism engaged 3 times (69 s total); the floor is the finish's
  own drain minimum, so live scratch settles ~8.6 GB regardless: the budget
  controls the WAVES above the floor, and the journal says exactly that.
  `PEAK_DISC_USED_BYTES 9304623877` (the whole tree at its widest, incl. the
  container being streamed).
- **Wall:** 2,702.89 s (45 min) for the full stack — sorter, finish, the
  boundary-aware trie, the m20 in-place join, the m15 in-place embed.

## X3 — consult end-to-end over 12,500+ windows (PASS)

Container = the X2 container. `gates/no_cross_x3_consult.sh`; rows parsed and
compared in-gate; the oracle is `gates/nocross_oracle.c` — a THIRD
implementation of the law (memmem, one multi-pattern pass over the whole
10 GB, the starts table bisected in plain C).

Arm 1 — **12,500 windows** (docs ≥ 4096 B, `start=1024, len=4, left=1024`),
50,000 positions, every histogram at full spine depth:

```
CONSULT_DONE positions=50000 windows=12500 ... boundary_exact_rows=50000 boundary_inexact_rows=0
BOUNDARY_INEXACT rows=0 total=50000 ratio=0.000000000 law=no_match_may_cross_a_document_boundary ...
X3_ARM1_EXACT boundary_exact=1 rows=50000 marked=0
X3_DOCID_BASE_DEPTH rows=50000 mismatches=0
X3_EXCLUDE rows=50000 excluded=50000 result=PASS
```

Every one of the 50,000 histograms is boundary-exact (no row carries the
conflated fallback), `width == sum(pairs) + eof` holds on every row, docid /
base / depth agree with the map on every row, and `--exclude-source` is
exact on every row (width−1, the own byte's pair decremented, every other
pair unchanged). Throughput 379–396 positions/s at `-j 32` (box under load
30–44; the pre-law A2/A3 reference was 4,556/s at `-j 96` on the 100 GB
container — the exact default pays for the backward search + per-occurrence
classification; the marked fallback path keeps the old rate).

Arm 4 — **brute-force spot checks, 60/60 exact:**

```
X3_ORACLE_SPOT_CHECK positions=60 exact=60 mismatches=0 result=PASS
```

Arm 2 — **the zone-(c) incidence, measured and LOUD** (the short-context
ramp, `start=1, len=16`):

```
X3_RAMP rows=1600 ctxlen<=8=800 marked_inexact=169 ratio=0.105625 cap=8192-default
X3_RAMP_LOUD marked rows exceed 1% of the ramp arm - the trie depth / cap defaults need revisiting
X3_RAMP_CAP256 rows=8000 marked_inexact=1777 ratio=0.222125 note=cost-knob arm
```

The measured cost basis: a block decode is ~12.6 µs on this box at 10 GB
(`decode_ns/block_decodes` from the journals), so a locate is ~13 ms and a
width-W enumeration costs ~13·W ms. At the default `--boundary-cap 8192` the
ramp runs at **1.27 positions/s** (786 ms/position at `-j 32`) with 10.6 % of
ramp rows past the cap and marked; at `--boundary-cap 256` it runs at
41 positions/s with 22.2 % marked. **The trade is real and the knobs are the
contract:** `--boundary-cap` is the cost/exactness knob per run;
`--boundary-strict` converts the fallback to a refusal; `--cross-doc` opts
the whole run into the legacy conflated answer. For a flagship-style T2048
cache build over whole documents, the default cap is NOT the right setting
for the ramp — either a smaller `--boundary-cap` (marked rows are cheap and
flagged per row) or the future reversed-tail member (below). The full-depth
spine — the cache build's real payload — is exact everywhere.

## X4 — search: the excluded set is exactly {rank(s) != rank(e-1)} (PASS)

`gates/no_cross_x4_search.sh`: 320 patterns (130 forty-eight-byte, 70
sixteen-byte near doc starts, 40 twenty-four-byte, **120 boundary-straddling**
24-byte patterns centred on a document start), 4 refused by `--max-occ`
(listed, never truncated):

```
SEARCH_BOUNDARY mode=no-cross-default structure=member-m15 patterns=320 absent=0 refused=4 excluded_cross_doc=123 matches_kept=15936 verified=16059 wall_s=657.758 ...
SEARCH_BOUNDARY mode=cross-doc structure=member-m15 patterns=320 absent=0 refused=4 excluded_cross_doc=0 matches_kept=16059 verified=16059 wall_s=516.403 ...
X4_EXCLUDED_SET patterns_with_output=316 default_rows=15936 cross_rows=16059 crossing_excluded=123 exact=1
X4_RESULT PASS
```

Every emitted match in both arms was **verified byte for byte against the
corpus** (`--corpus`, pread + memcmp per match). The 123 excluded matches are
exactly the `cross=1` rows of the `--cross-doc` arm, exactly the matches whose
independent starts-table classification says a document start lies inside
`[s, e-1]`, and the default output contains all and only the cross=0 matches.

## X5 — no regression (PASS)

`gates/no_cross_x5_regression.sh` (see its log for the verbatim lines):

- **The separator corpus path (pile.txt) unchanged:** the plain 150 MB build
  is byte-identical to the banked adadf48-era container
  (`871c904f3a67856943a652d1bad743b826cf6892e2265852d9b4f619ff1f98d5`), and
  the `--docs-map --no-trie` build is byte-identical to the banked
  m15-embedded container
  (`dd56e422811c55374ded1a4c703aeff0219427be86a90e55bb371f0ecec99faa`) —
  m15 embedding is byte-for-byte what it was, and the unaware trie build is
  count-identical to the old builder (the law is classification, never a
  stream edit).
- **chi unchanged** at 150 MB and asserted at 10 GB (X2).
- **cargo clean + full release build + `cargo test`: 98 passed, 0 failed**
  (1 ignored, the pre-existing selftest sweep marker), plus
  `ctx-selftest`/`ctx-tap-selftest` above.

## What landed

- **The trie (m20, member format v3):** the law by construction — per-row
  classification against the document end, non-crossing occurrence counts,
  crossing occurrences counted nowhere, END = "this context ends a document
  (or the text)", `boundary_aware` journaled in the member head; built
  boundary-aware whenever `--docs-map` names a starts table (both shapes:
  pile-map dir and CORPUS V2 `.starts`); the post-hoc `ctx-build` reads the
  container's own m15; unaware members say `boundary_aware=0` and
  `branch-of`/`trie-stat`/`relax`/`relax-stat`/`batch` REFUSE boundary
  answers against them unless `--cross-doc` (which an aware member refuses —
  never a silent semantic switch). `ctx-verify` oracles the law and refuses
  an aware m20 whose container lost m15.
- **consult (row file v3):** the layered exact default (trie → enumeration →
  MARKED fallback), `eof` = doc-ending occurrences + the structural END,
  flag bit 4 = `boundary_exact=0`, `--cross-doc`, `--boundary-cap` (8192),
  `--boundary-strict`, `BOUNDARY_INEXACT rows=.. total=.. ratio=..` on every
  run, CONSULT.md §5.4. `--doc-restrict` gains the same END rule (a doc-end
  occurrence is END, not dropped).
- **search:** `xsa search` (new, qlite kernel) — locate every occurrence,
  classify, exclude crossing by default, `--cross-doc` returns them marked,
  `--corpus` verifies every match byte for byte, `--max-occ` refuses by name,
  `SEARCH_BOUNDARY` journal. The legacy `query`/`mems` path applies the same
  predicate where m15 exists and now READS the modern lite shapes (v6..=10,
  m12/m13, flags 128/256/512) it silently rejected before.
- **branches (serve kernel):** default = exact enumeration via m15 + `--corpus`,
  widths past the cap REFUSED (a serve frame has no marking bit), containers
  without structure refused unless `--cross-doc`.

## Residual risk, priced

The only case without a cheap exact answer is a context **wider than the trie
and with more occurrences than `--boundary-cap`**: the boundary correction is
a join between the match set and the doc-start metadata, invisible to rank
arithmetic. Consult answers those rows with the legacy conflated histogram,
MARKED per row and journaled per run (10.6 % of ramp rows at the default cap
on Pile text; 0 % at full spine depth). The proper fix is a future
**reversed-document-tail member** (~1 % of corpus: each document's last ≤ W
bytes as a trie over reversed tails with per-node next-byte histograms —
answers the END count and the per-byte split directly, no locate at all);
priced in MEMORY_OPS.md. Until then `--boundary-cap`, `--boundary-strict` and
`--cross-doc` are the honest knobs, and every row says which of them produced
it.
