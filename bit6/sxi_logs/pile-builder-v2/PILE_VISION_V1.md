# THE PILE VISION — the coherent builder (v1, locked for operator confirmation)

Author: supervisor. Status: PROPOSED — awaiting operator confirmation before any
implementation. Supersedes the accreted shape for the byte-level pile builder
only. The pangenome side (PFP route, 466 haplotypes) is DONE, BANKED, FROZEN —
nothing in this document touches it.

Inputs taken as given:
- Pile: n = 1,307,910,802,540 bytes, 0x1E-separated documents, cyclic
  single-string convention, one remap (the involution; 0x1E fixed; 1-4 vacated
  as the dollar guard band; ALL other bytes legal text, including 0x00/0x05).
- Product: the lite container {m1 runs, m4 anchors, m5 chi EF, m11 sparse
  1/1024 tails} = 0.468n ~= 612 GB. Member 2 stays warm-side (not built here).
- Hardware envelope (operator-fixed): ~100 GB RAM for sorting/organization,
  ~2 TB working disk, 96 threads, NVMe. The corpus file itself is not scratch.
- Bar: 5-day pile wall = ~330 s/GB end-to-end. If a clean measurement says we
  cannot reach it, the residual's anatomy gets reported, not patched around.

## THE DOCTRINE (binding on every change)

D1. R SPACE ONLY. The only text that exists is the corpus file, read through
    windows (mmap of bounded regions, never the whole 1.31 TB range; RLIMIT_AS
    respected by construction). No T||T, no m2.bin, no pftext/pfck, no side
    copies. LCE reads bytes from the corpus through the adapter.
D2. NO DENSE POSITION COLUMNS. No array of 64-, 41-, or 32-bit integers with
    one entry per suffix, ever, at any phase (this is what the 4-8x blowups
    all were: SA, PLCP on doubled text, WM = merged SA, order vectors).
    Positions appear only as O(r) sparse per-run samples that the PRODUCT
    itself serializes (m11 1/1024 tails; m4 anchors) or as O(r) checkpoint
    hints consumed on the fly and deleted.
D3. FIXED BUDGETS, ENFORCED: one shared RAM envelope for the whole pipeline
    (merge + finish together, not two full caps), hard ulimit, and a scratch
    ceiling of ~1.5n. A design that needs more is wrong, not unlucky.
D4. SCRATCH IS FUEL, NOT ARTIFACT: produced in bands, consumed in bands,
    deleted on use. No new durable file may be invented by an implementation
    agent. If a change adds a durable artifact instead of removing one, it
    is going the wrong way.
D5. VERIFY THE CORE, THEN BUILD OUT. The mandatory verification pyramid is
    exactly: (a) tiny brute-force oracle (runs, chi, sampled witnesses) on
    synthetic + real windows INCLUDING 0x00/0x05, nonperiodic wrap-prefix
    cases ([0x1e,0x0a,0x1e] class), periodic chunks, nested prefix intervals;
    (b) 100 MB and 1 GB: byte-identical container vs the banked reference,
    chi exact; (c) at scale: chi agreement across two independent shapes +
    sampled witnesses + container structure checks. Everything else
    (double seam evidence, full-rlebwt decode-replay, pieces re-derivation)
    is a DEBUG TOOL, off the critical path.
D6. NO REGRESSION IN SEMANTICS: same container bytes at the fixtures, same
    chi definition, same tie rules. Every lever lands byte-identical at
    100 MB / 1 GB or it does not land.

## THE SHAPE (what the pipeline becomes)

Phase 1 - CHUNK (text -> per-chunk cyclic run streams):
  libsais on the doubled chunk (retained: the ordering argument is proven),
  but: PLCP DROPPED via the minimal cyclic period p (already computed for
  .pos): p = n needs no tie repair at all; p < n emits residue classes mod p
  ascending. Order projected IN PLACE into the SA array; runs and sparse
  per-run samples STREAMED to disk as they are produced. Major arrays:
  ~10-11n per worker (was 27n) -> ~4-8 workers in the envelope (was ~2).
  Chunks stay <= 1 GiB (int32 ceiling); ~1,219 chunks; delete-as-you-go.
  Scratch: compact run set ~0.75n.
Phase 2 - MERGE (k-way fan-in, R space):
  Comparator reads the corpus through bounded windows with the remap applied
  on read; virtual T||T via modulo coordinates (no m2.bin). Fingerprint
  checkpoints sparse (CROSS_HASH_K tuned; they are HINTS, every proposed LCE
  still byte-verified - the exactness guarantee stays). Level output = next
  level input; delete-as-you-go; no re-derivation.
  Emission: shard-local run streams + counts; global run bases stitched;
  seam occurrence targets discovered BEFORE the walk, their rows/flanks
  captured WHILE rows stream, spooled small. NO whole-WM file (the merged-SA
  column is gone - it was 5.125n = 6.7 TB; the finish's consumers get the
  seam spool + streaming counts instead). Peak merge scratch target:
  ~0.5n beyond the surviving level inputs.
Phase 3 - FINISH (fused: seam repair, LCE, witnesses, container):
  Witness enumeration writes SORTED runs directly to spill (compressed,
  measured 0.125x) and the EF encoding consumes them via a streaming k-way
  external merge - buckets are NEVER all resident (was: ~3 TB RAM). All
  bit writers are FILE-BACKED (spool + CRC at close; was: whole m5/m1
  bitstreams in RAM, 256-722 GB). Corpus access via bounded mmap windows
  with explicit fallback that logs loud (was: whole-corpus map that silently
  degrades under RLIMIT_AS). One completion sentinel replaces the full
  rlebwt decode-replay. Per-band worker pools persistent, not respawned.
Phase 4 - PUBLISH: the container once, sequential write, CRC per member,
  then scratch tree deletion. rlebwt intermediate deleted post-publish.

Budget arithmetic (the operator's envelopes, checked):
  RAM: chunk workers ~8 x 11n(1 GiB) ~= 88 GB, merge+finish within the same
  envelope by scheduling; hard ulimit on the wrapper.
  Scratch: run set 0.75n + one merge level of surviving inputs (<= 0.75n,
  delete-as-you-go) + windows/spools ~0.05n + witness spill ~0.04n
  = ~1.6n ~= 2.1 TB. If any term overruns, the design shrinks it, the
  operator is not asked for more disk.
  Wall (allocation to be RE-ANCHORED by measurement, not asserted):
  chunk 60 + merge 150 + finish 100 + publish 20 = 330 s/GB = 5.03 days.
  The audit's honest planning range was 300-620 s/GB (4.5-9.4 days); 330 is
  the target end and is credible ONLY with all levers landed; the first
  clean 1 GB measurement after lever 1+2 publishes the real number and the
  residual anatomy if it is above 330.

## THE IMPLEMENTATION SERIES (five runs of the sole agent sxgc.codex-sol-writer / gpt-6-sol, one lever each, gated)

Each agent: sxgc.codex-sol-writer (cheap, strong), sole, given THIS document +
the audit (AUDIT_2026-10-13_codex_astra.md) + its charter below. Gates per
agent: D6 byte-identity at 100 MB/1 GB + the D5 pyramid; no new durable
artifacts; no verification layering without supervisor sign-off. Supervisor
audits and merges after each.

  sol-1  FINISH BOUNDED: streaming witness merge (external k-way over spill
         runs, never all resident), file-backed bit writers, windowed corpus
         mmap with loud fallback, completion sentinel, persistent workers.
         Kills the ~3 TB + 256-722 GB RAM blockers. (The pile cannot launch
         without this; it is first.)
  sol-2  MERGE R-SPACE, PART 1: virtual corpus comparator (no m2.bin),
         modulo-coordinate wrap, remap-on-read, prompt dead-scratch release,
         checkpoint sparsity tuned. Kills 2n of text on disk; THE LAW
         restored at the merge.
  sol-3  CHUNK FRONT END: PLCP dropped via period-p grouping, in-place order
         projection, streamed runs/samples, 27n -> ~10-11n. (Low risk, exact;
         more workers.)
  sol-4  MERGE R-SPACE, PART 2: shard-local emission + seam capture, kill
         whole-WM + its consumers' dependency, stitch global bases.
         (Highest protocol risk; needs the seam spool re-gate. This is what
         dissolves the old 13-20 TiB hardware question - permanently.)
  sol-5  CLOSE-OUT: verification slimming per D5 (debug tools off the
         critical path), plan model rebuilt from measured numbers, the 10 GB
         re-rung (chi cross-checked via the v1 path on the same encoding),
         and the launch table: wall, peak, transient, provenance - the
         operator's go/no-go for the pile launch.

Order: sol-1, sol-2, sol-3 (independent, may interleave), then sol-4, then
sol-5. The 10 GB re-rung can be re-attempted after sol-1 + alphabet closure
(it fits the current disc); the pile launch waits for all of them.

## FROZEN / OUT OF SCOPE

- The pangenome PFP route, 466, warm container, GWAS/fork-matrix: banked,
  untouched.
- The product members' formats and the container bytes (byte-identity gates
  depend on them).
- The one remap (involution) and the 0x1E separator.
- The banked constants at 100 MB / 1 GB. (The old 10 GB chi constant's era
  question is resolved by sol-5's re-banking with cross-check, per
  REMAP_ERA_FINDING.md - the product gates at fixtures are the oracle.)

---

# ADDENDUM (2026-10-19): THE BIN ERA — what the week measured, what it killed, and what the machine is now

This document is still the doctrine (D1-D6 stand, D2 with the rolling amendment
below). What changed is the SHAPE: the chunk -> merge -> adopt chain this file
describes was retired by measurement, and the pile builder is now a different
machine. This addendum is the ledger of that pivot, written so the next reader
does not have to re-derive it, and so the retired parts are not mistaken for the
plan.

## 1. Why the comparison merge retired

The merge walk's cost was never the comparison count; it was the POSITION STORE.
Every shape that could certify a merge needed per-suffix positions on disc
(dense 8n or compact 3.6n) and re-derived them at ~0.7-1.6 s/MB
(`RATES_V2.merge_load_derive_s_per_mb`), i.e. 13-20 TiB of hardware for the pile.
Four separate redesigns (WM removal, checkpoint sparsity, shard-local emission,
design B) each removed a term and left the same shape: a global merge that must
consult positions it does not have.

The bin machine removes the question instead of answering it. A byte-1 bin's rows
ARE a contiguous slice of the global BWT, so CONCATENATION of the 256 bins in
byte order is the BWT: there is nothing to merge, no global order to consult, no
per-suffix position column anywhere. That is why the merge walk is gone and not
merely tuned.

## 2. The ledger: measured, killed, kept

| # | measured | verdict |
|---|---|---|
| 1 | Serial fill sweep: 316 MB/s, 655 windows x 1.15 h = **754 h** at pile (bin-depth §4) | KILLED by the prefix-sum scatter: 213.006 s -> 4.045 s at 10 GB/6 windows (52.7x; 21.1 GB/s on a quiet box, 75x the serial per-pass rate). Fill share of the construction term 33.3% -> 1.0%. |
| 2 | Serial comparison merge + position columns (13-20 TiB pile hardware) | RETIRED: replaced by the bin concat + ordered stitch. No position column exists in the product path (`full_SA=0 persistent_positions=0`). |
| 3 | `--bin-level-bytes 2` as a 65,536-code table (10x worse CPU) | KILLED: the top level is two SEQUENTIAL 1-byte levels (257 codes each). The flag is now free: 1.402 s vs 26.045 s at 100 MB, 24.86 s vs 151.79 s at 1 GB. |
| 4 | `--bin-depth-cap 32` as the recursion's rule | DEMOTED to a fail-loud backstop. The depth cap was deciding where leaves landed, and at 10 GB it took a leaf with **hundreds of millions of rows** to a single-threaded comparison sort (measured `thread_max_s=148.71` inside the hot bin, i.e. one thread for 149 s). A cap can only ever be right for one corpus; the target size is right for all of them. |
| 5 | The finished-artifact fold, serial per band | DECOMPOSED: the fold rides the stitch and the verification fans out per band. Finish own work 211.8 -> **118.9 s/GB** (the pile's 43.3 h term). |
| 6 | The 1.31 TB corpus cannot be built unsharded (3.48 TB of arena pair for a single 217e9-row bin) | TRANSFORMED, not solved: the window is a bounded ROLLING FILE now (8 B/slot on disc, `credit` of them live, unlinked when built), so the window is 0 B of RAM. A BIN is still indivisible (its slice is sorted in place), so the RAM term is one build group - which is why sharding at ~250-340 GB per shard remains the launch shape. |

## 3. The machine as it is (auto-radix, `xsa build`, one entry)

1. **The multi-pass reader.** Pass 1 counts bytes per corpus chunk per bin
   (`hists[t][c]`), 64-way parallel, one full pass. That count is enough to
   prefix-sum every (bin, chunk) region boundary, so pass 2 - the fill - is a
   PARALLEL SCATTER: each thread writes its own chunk's positions into its own
   regions, no lock, no cursor, and concatenated in chunk order the regions hold
   each bin's positions in ascending position order (asserted, not assumed).
2. **Rolling window FILES.** A window's positions live in a bounded file in the
   scratch tree (`bw-w<k%credit>.pos`, a 4 KiB header + one 8 B arena word per
   admitted slot). The fill writes through a MAP_SHARED mapping - the same
   pointer the RAM arena gave it - and the build reads each bin's slice back with
   ONE sequential bulk pread. A window file carries a completion word; a missing,
   truncated, stale or corrupt one is a HARD REFUSAL, never a silent rebuild
   (five sabotage modes are exercised by the oracle).
3. **Target-size recursive bins.** The top level's byte-1 masses come free from
   the count pass. Any bin holding more than `--bin-target-size` rows subdivides
   by its NEXT BYTE - count within its own slice, prefix-sum, scatter into the
   arena/scratch pair - and recurses. Depth is an OUTPUT: a bucket whose rows
   keep sharing prefixes keeps being divided one cheap coded symbol at a time,
   and only a node that is already small (or whose suffixes end inside it) becomes
   a leaf, an LCP-carrying comparison sort from the certified depth. The depth
   cap survives only as a fail-loud backstop for a corpus that cannot be divided.
   Measured (1 GB, 96t): leaf rows are bounded by the target (the old cap left a
   4.7M-row leaf and 183,229 over-target leaves; the target leaves none), and
   build is 15-20% faster at a 256K-1.3M target than at the retired 24-row rule.
4. **The ordered stitch IS the merge.** Bins are published in byte order into the
   unchanged band/piece protocol by one sequential pass that coalesces runs,
   numbers them, closes 65536-run bands and hands each band to a writer thread;
   the run's position, BWT symbol and capture-target flag already travel in the
   arena word, so the stitch never reads the corpus at random and never hashes a
   row.
5. **The decomposed finish** (`pile_finish`) consumes those pieces CONCURRENTLY,
   flow-controlled through the piece window, and publishes the lite container,
   then deletes the rlebwt.

**Pile arithmetic as it stands** (1.3079 TB, 5 shards, measured anchors only):
build ~12.9 h + fill ~0.5 h + stitch/finish **43.3 h** is ~57 h end to end, i.e.
finish-dominated. The launch limiter is the finish term and the per-shard arena
capacity, NOT the fill (which this week's scatter removed) and not the merge
(which no longer exists).

## 4. D2, amended (the rolling amendment, operator-directed)

D2 stands as written: no persistent per-suffix position column, ever, at any
phase - no SA, no PLCP on doubled text, no WM, no order vectors. The amendment is
about TRANSIENT materialization: suffix positions MAY appear in bounded rolling
segments (the window files) during the read/scatter/bucket passes. The
implementation must enforce a numeric slot cap, release each segment before the
next, and journal the high-water mark: `peak_live_slots`, `peak_live_bytes`,
`peak_window_file_bytes`, `window_files`, `window_loads`, `win_file_write_bytes`,
`win_file_read_bytes` are printed on every run, and the oracle asserts the bound
(`peak_live_slots <= 2*max(cap, n)`, `peak_window_file_bytes <= credit*cap*8`).
D1 is untouched: the corpus file is the only text; every text read goes through
`CorpusText` windows and nothing text-sized is ever written.

## 5. THE PFP-RADIX DOMAIN MAP (operator's idea; the pangenome-side sequel)

The machine's structure is: a COUNT pass over a corpus that produces per-key
masses; a PREFIX-SUM; a PARALLEL SCATTER into per-key regions; bounded rolling
window files; target-size recursion inside a key's slice; concatenation in key
order as the order. The only thing that changes between domains is WHAT THE RADIX
TOKEN IS:

* **byte radix (this machine, built and gated) = web text.** The token is a byte
  of the corpus, the "dictionary" is the text itself. This is the regime the pile
  is in, and it is what `xsa build` runs today. Byte depth 3-200 gets a leaf under
  the target; that is the whole story for text.
* **PHRASE radix = cancer cohorts / raw read collections / structural-variant
  pangenomes.** The token is a PHRASE from a PFP dictionary (a phrase is a
  superbyte), so the alphabet is ~10^6-10^8 tokens instead of 256. Near-identical
  clone sets (tumour/normal pairs, cohorts of the same locus) differ in single
  bases inside otherwise identical multi-kb blocks: under byte radix they stay in
  the same bucket for 6-10 levels and every level costs a random read of a
  near-identical block; under phrase radix the SAME divergence becomes a branch at
  PHRASE DEPTH 1-2, because the dictionary already resolved the shared text into
  one token. Raw read collections behave the same way: the reads ARE phrases from
  a small dictionary, which is why PFP compresses them at all. Structural-variant
  pangenomes add the same argument one level out: haplotype blocks are near-
  identical phrases with rare edits.
* **What carries over unchanged:** the count/scatter prefix-sum (per phrase
  instead of per byte), the bounded rolling window files (8 B per position,
  unchanged), the target-size recursion (the leaf bound is rows, not bytes, so it
  transfers verbatim), the ordered stitch (concatenation in phrase order is the
  phrase-order BWT), and the decomposed finish. What changes is the tokenizer and
  the census table's width - not the machine.

This is marked as the SEQUEL: the byte machine is the thing that is built, gated
and measured this week, and no phrase-radix claim ships until it has a census, an
oracle and a byte-identity gate of its own.
