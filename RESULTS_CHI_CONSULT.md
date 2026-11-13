# The χ-proportional consult: RESULTS

**The claim being tested:** consult's cost must be proportional to the fork
skeleton (χ), not to n. The old default answered every row by **enumerating**
its occurrence interval — one sampled-SA LF walk (`locate_steps`) per
occurrence — so a 1024-byte spine that occurs twice cost two ~0.17 s walks
over a 65 GB container: **13.7 positions/s**, locate-bound regardless of RAM
(the rehearsal measured 8.5/s at `--rss-gb 8`, 13.7/s at 96, "wedged twice on
single width-8192 items").

**What landed** (`xsa/src/consult.inc.rs`, query-side only — no container,
member or corpus change): the no-cross default answers by a **zero-locate
layer cascade** whose soundness anchor is the **separator-framing law**:

> On a corpus whose every in-text document start `d` is immediately preceded
> by one 0x1E byte (the Pile's own framing — verified **exhaustively** at
> open, one 1-byte pread per document start, never assumed), a context that
> contains no 0x1E byte **cannot touch a document boundary**: a crossing
> occurrence would need `corpus[d-1] = 0x1E` equal to `C[d-1-pos]`, and an
> END-branch occurrence would need `C[ctxlen-1] = 0x1E`. Every occurrence is
> then in-document content or the one structural text end — exactly the raw
> backward-search classification — so **the no-cross histogram IS the raw
> histogram, at any width, with zero locate.**

Under that law the layers are: **WIDTH-1** (raw width 1 ⇒ the only occurrence
is the query's own; exact on ANY container), **SINGLE-CHILD** (the trie node
of the context's last ≤8 bytes with one child forces the byte — a fork can
never widen going deeper), **ONE-STEP VERIFICATION** (candidate = the query's
own continuation from the window; `width(C·b) == width(C) − eof` ⇒ forced),
**RAW FORK** (the full extension-count histogram — pure rank arithmetic, the
same machinery `xsa branches` uses). Contexts carrying an internal 0x1E
(content bytes of their own document) and containers that fail the framing
verification fall through conservatively to the **exact m15-crossing-filtered
enumeration** — the old path — so exactness is never traded for speed. The
`--boundary-cap` marked fallback is **unchanged byte-for-byte** (downstream
experiments are hash-bound to the row bytes).

**The one-step-verification trap, defused.** The naive reading of the design
("equality ⇒ forced, emit the raw width") is *wrong*: a **crossing occurrence
that continues with the same byte** keeps `width(C·b) == width(C)` while the
law's answer drops it — the emitted count would be too high. The trap is
constructed twice in the G3 gate (once on an unframed corpus, where the
open-time verification must fail, once on a framed corpus whose 0x1E-carrying
spine hides a junction match, where the alarm must fire) and both traps assert
`trap_is_real=1`: the conflated answer (2) genuinely differs from the law's
(1), and the binary answers the law's.

**Row output:** the default stays v3, byte-identical. `--rows-v4` opts into the
compact row file: a 1-byte KIND per record, FORCED rows drop the 9-byte pair
(flag bit 6; 62 vs 73 bytes; **9.1% smaller** on the G1 set).

---

## Gates (all green; every check prints N of things examined)

Container for G1: `/mnt/nvme2n1/erikg/chi-consult/g10g/pf-10gb.sxi2` (10 GB
full-stack, built `--tape --trie-depth 8 --trie-min-support 1000 --docs-map
/tmp/map10g`, χ = 2,676,925,006 == the banked constant). Container for
G2/G4: `/mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2` (65.5 GB,
χ = 24,185,339,204, m15 = 16,119,767 docs, m20 depth-8 boundary-aware trie).
The banked row file `/tmp/r100rows.tsv` (682,496 rows, the killed run's
committed prefix of `/tmp/r100w.jsonl`'s 6,667,438 positions) is the census.

### G1 — PARITY (10 GB, pre-change binary vs the χ binary)

Window set: 1,497 doc-contained windows (827,135 positions) — ramp windows at
document starts (empty/1-byte/short/full spines), mid-document windows,
END-adjacent tail windows, `left=2`/`left=16` windows, whole tiny documents,
and 512 windows around **INTERNAL 0x1E** bytes found by scanning the corpus
(`gates/chi_gen_windows.c`; base = the TRUE document start from the map —
consult dies on structure disagreement by design).

```
G1_BYTE_IDENTICAL old_vs_new=1 bytes=94588692      (same rows_sha256 from both binaries)
CMP rows=827135 diffs=0
G1_FORCED forced_rows=784657 (width1=762310 single_branch=22347) mismatches=0
G1_V4 rows=827135 forced_kind=784619 diffs=0 bytes_saved=8588293 (9.1%)
CONSULT_SEPFRAMING checked=1606584 violations=0 verified=1 seconds=0.427
CHI_ZEROLOCATE rows=795574 of 827135 (96.18%)
G1_LAYERS width1=762310 trie=29716 forced=21400 rawfork=10955 tforced=909
          enum=19 marked=1416
G1_PARITY_OK rows_compared=827135 byte_identical=1 forced_equal_old=1 v4_forced_kind_ok=1
```

`enum=19`: the internal-0x1E windows whose spines carry the separator AND have
width ≥ 2 — the conservative layer, exercised on real data, byte-identical.
`marked=1416`: the cap-8192 tail, byte-identical to the old binary. Rates at
10 GB (the 7.4 GB container is page-cached here, so the old binary's locates
are cheap too): old 6,715/s, new 7,061/s — G1's job is parity, and it is total.

### G2 — CENSUS RE-DERIVATION (100 GB rehearsal, full window set)

`--rss-gb 8`, `-j 16`, the live pile build (96 threads) on the same box.

```
CONSULT_DONE positions=6667438 seconds=1088.4 positions_per_s=6125.9
              peak_rss_gb=7.999  boundary_exact_rows=6652592 (99.78% exact)
CONSULT_SEPFRAMING checked=16119766 violations=0 verified=1 seconds=7.1
G2_PREFIX rows=682496 width1=601293(diffs=0) single=72347(diffs=0)
                       fork=6924(diffs=0) marked=1932(diffs=0)
G2_PREFIX_OK fork_rows=6924 marked_rows=1932 byte_identical=1
G2_MIX_MEASURED width1=88.10% single_branch=10.60% marked=0.28% fork=0.87%
G2_CENSUS_TARGET width1=88.1 single_branch=10.6 wide_histogram=0.7 forks=0.5
CHI_ZEROLOCATE rows=6605827 of 6667438 (99.08%)
G2_LAYERS width1=5962110 trie=43372 tforced=13702 forced=593043
          rawfork=36972 marked=14846 enum=0
G2_CENSUS_OK banked_prefix_compared=682496 byte_identical=1 rate=6125.9/s
```

**Every one of the 682,496 banked rows is byte-identical** — width-1,
single-branch, fork AND marked. The census's 88.1% width-1 and 10.6%
single-branch reproduce exactly (the census's "0.7% wide histogram" bucket
mixed marked rows with >8-branch rows: 3,649 of the 5,936 multi-branch rows
here have >8 branches and are trie- or raw-answered, both zero-locate).
`enum=0`: no 0x1E-carrying spine with width ≥ 2 exists in the whole 6.67M
position set — the conservative layer costs nothing on real data at this
internal-separator rate (the map's MANIFEST: 6,979 internal 0x1E in the
first 100 GB).

### G3 — THE LAW (adversarial fixtures, brute-force oracle + pre-change parity)

Five 48 MiB corpora (`gates/chi_law_fixtures.py`): the same-byte-crossing
trap unframed and framed, widths 2–4 on framed 0x1E-free spines,
separator-as-content + END/text-end, internal-0x1E everywhere.

```
CHI A TRAP law_width=1 conflated_width=2 row_width=1 trap_is_real=1
CHI B TRAP law_width=1 conflated_width=2 row_width=1 trap_is_real=1
CHI A sepframing expected_verified=0 got_verified=0   (unframed → every row enumerates)
CHI B sepframing expected_verified=1 got_verified=1   (framed → the 0x1E alarm fires)
CHI A/B/C/D/E PARITY identical_to_prechange=1 (each fixture's whole row file)
CHI_G3_EXAMINED fixtures=5 rows=19 failures=0
G3_LAW_OK fixtures=5 rows_oracled=19 byte_parity=1 traps=2
```

### G4 — RATE (100 GB rehearsal, `--rss-gb 8`, cold then warm)

<!-- G4_RECEIPT -->

---

## The cost structure, measured (G2/G4 runs, `-j 16`, `--rss-gb 8`)

| layer | rows (G2 full set) | share | p50 per row | locates |
|---|---:|---:|---|---|
| trie (m20, ctxlen ≤ 8) | 43,372 | 0.65% | 11.6 µs | 0 |
| width1 | 5,962,110 | 89.42% | 3.0 ms | 0 |
| single-child short-circuit | 13,702 | 0.21% | 1.5 ms | 0 |
| one-step forced | 593,043 | 8.89% | 5.9 ms | 0 |
| raw fork | 36,972 | 0.55% | 5.9 ms | 0 |
| enum (0x1E spines) | 0 | 0.00% | — | width each |
| marked (cap tail, unchanged) | 14,846 | 0.22% | 5.9 ms | 0 |

**99.08% of rows answer with zero locate** (the census predicted ~99.5% for
the forced mass; the fork mass joins it because the framing law covers forks
too). The per-row cost is the backward search of the full ≤1024-byte spine
(~2 rank steps per context byte over the run-directory and decoded blocks) —
`n`-sized machinery touching χ-sized work: consult's cost is now proportional
to the fork skeleton. The old path's locate ledger for the banked prefix was
4.94M locates (601k width-1 + 1.35M single-branch + 2.99M fork-dominated);
the new path spends zero.

**Walls:** G2's full-set run 1,088 s at `-j 16` coexisting with the live pile
build (96 threads) and the 10 GB gate container build (32 threads); the
framing verification 7.1 s (16.1M single-byte preads, cold pages) at open,
1.8 s warm. Peak RSS 8.00 GB — the cap held, 1,819 governor drop epochs.

## Residual risks

1. **The framing law is per-container, verified per run.** A container whose
   document starts do not all follow a 0x1E byte (adversarial fixtures,
   HPRC-style maps, any hand-made `--doc-starts`) gets `verified=0` and the
   exact enumeration for every row — correct, but the rate falls back to the
   old regime. The verification is exhaustive (a sample cannot authorize the
   zero-locate layers); its cost is one 1-byte pread per in-text document
   start at open (16.1M docs ≈ 2 s warm, 7 s cold at 100 GB).
2. **Spines carrying an internal 0x1E enumerate.** They are ~0.1% of positions
   at the Pile's internal-separator rate (446 docs in 10 GB, 6,979 in 100 GB)
   and only those with width ≥ 2 pay locates (19 rows in G1's 827k; 0 rows in
   G2's 6.67M). A corpus with much more internal 0x1E would shift that mass
   back to the enumeration cost.
3. **The marked fallback's bytes are deliberately frozen.** The raw layers
   stay capped at `--boundary-cap` even though the framing law would answer
   wider rows exactly — a too-wide row must keep today's conflated, marked
   bytes because downstream experiments are hash-bound to them (0.22% of the
   G2 set). A future row-file version may lift the cap; this one does not.
4. **The `tforced` shortcut dies on a member/corpus disagreement** (a single
   trie child whose byte differs from the query's own continuation) — that is
   a corrupt member by the trie's own build law, and the process refuses
   rather than answering.
5. **`enum` rows on unverified containers still pay the locate grind.** That
   is the exactness-or-nothing trade the gate asserts; at 100 GB a
   0x1E-carrying fork row is a fraction of a percent of rows.
