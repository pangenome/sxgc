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

## THE IMPLEMENTATION SERIES (sole agents, one lever each, gated)

Each agent: sxgc.codex-sol-writer (cheap, strong), sole, given THIS document +
the audit (AUDIT_2026-10-13_codex_astra.md) + its charter below. Gates per
agent: D6 byte-identity at 100 MB/1 GB + the D5 pyramid; no new durable
artifacts; no verification layering without supervisor sign-off. Supervisor
audits and merges after each.

  SOL-1  FINISH BOUNDED: streaming witness merge (external k-way over spill
         runs, never all resident), file-backed bit writers, windowed corpus
         mmap with loud fallback, completion sentinel, persistent workers.
         Kills the ~3 TB + 256-722 GB RAM blockers. (The pile cannot launch
         without this; it is first.)
  SOL-2  MERGE R-SPACE, PART 1: virtual corpus comparator (no m2.bin),
         modulo-coordinate wrap, remap-on-read, prompt dead-scratch release,
         checkpoint sparsity tuned. Kills 2n of text on disk; THE LAW
         restored at the merge.
  SOL-3  CHUNK FRONT END: PLCP dropped via period-p grouping, in-place order
         projection, streamed runs/samples, 27n -> ~10-11n. (Low risk, exact;
         more workers.)
  SOL-4  MERGE R-SPACE, PART 2: shard-local emission + seam capture, kill
         whole-WM + its consumers' dependency, stitch global bases.
         (Highest protocol risk; needs the seam spool re-gate. This is what
         dissolves the old 13-20 TiB hardware question - permanently.)
  SOL-5  CLOSE-OUT: verification slimming per D5 (debug tools off the
         critical path), plan model rebuilt from measured numbers, the 10 GB
         re-rung (chi cross-checked via the v1 path on the same encoding),
         and the launch table: wall, peak, transient, provenance - the
         operator's go/no-go for the pile launch.

Order: SOL-1, SOL-2, SOL-3 (independent, may interleave), then SOL-4, then
SOL-5. The 10 GB re-rung can be re-attempted after SOL-1 + alphabet closure
(it fits the current disc); the pile launch waits for all of them.

## FROZEN / OUT OF SCOPE

- The pangenome PFP route, 466, warm container, GWAS/fork-matrix: banked,
  untouched.
- The product members' formats and the container bytes (byte-identity gates
  depend on them).
- The one remap (involution) and the 0x1E separator.
- The banked constants at 100 MB / 1 GB. (The old 10 GB chi constant's era
  question is resolved by SOL-5's re-banking with cross-check, per
  REMAP_ERA_FINDING.md - the product gates at fixtures are the oracle.)
