# PILE-BUILDER v2 lane: the lean streaming pipeline

The user reset the route: the verification discipline became the
architecture (every intermediate a durable comparable file = the scaffolding
being deleted, not optimized). ONE product persists: the lite container + the
chi constant. Intermediates are scratch: produced in bands, consumed in
bands, deleted on use. Branch `pile-builder-v2` from `0a39cf5`.

## The two approved decisions (supervisor, 2026-10-10)

1. **Lite container member set = {m1 runs, m4 anchors, m5 chi EF, m11 sparse
   1/1024 tails}** - member 2 (packed per-run tails) is DROPPED from the pile
   lite. Not a downgrade: the pile lite as originally specced is ~500-580 GB
   (~0.4n) and m2 at 1.38n = 1.8 TB could never fit in it. m2 is the WARM
   container's member (cheap at pangenome scale). Locate = m11 + anchored
   walk (measured ~3.1k mean LF steps on text-like pile data; improvable by
   the banked orbit-aware anchors).
2. **Corpus-backed LCE replaces merge-emitted pftext/pfck.** The LAW is
   "never materialize text to disk" - pftext IS text on disk (the merge-time
   emission was the one sanctioned exception). v2 restores the discipline:
   the corpus is the canonical text store; the finish reads it read-only
   through the adapter `T[i] = REMAP[corpus[i]]` with windowed preads.
   Safeguards: (a) the one-time full byte-compare of the adapter stream vs
   the banked 1GB `frag.pftext` (M1 below) before anything trusts it;
   (b) after that, the standing chi/sA byte gates cover it; (c) the adapter
   is read-only - the corpus is never written, moved, or remapped on disc;
   (d) the rotation/remap derivation is journaled here (there is NO rotation:
   the merge's `--emit-pf` text M is the remapped corpus byte stream).

## M1 - the adapter identity gate: GREEN (2026-10-10)

`adapter_verify_1b.sh`: full streaming compare of `REMAP[corpus[i]]` vs the
banked `mergperf-1b-B/merged/frag.pftext` (n = 1,000,000,665):

    compared 1000000665 bytes; remap-active corpus bytes: 224; mismatched blocks: 0
    ADAPTER_IDENTITY

The remap branch is exercised (224 padding-range bytes in the fixture) and
the identity holds byte-exact, including the 0x1E document tail. The tau
grammar for the finish's PFCK (built by the finish from the corpus, same
recurrence as the merge's `--emit-pf`) is
`tau = (8n + final_runs - 1)/final_runs` (banked 1GB header: tau=22,
count=45454576) - the finish's corpus-built PFCK must be byte-identical to
the banked `frag.pfck` (gate M3).

## The v2 pipeline (as being built)

1. chunk (unchanged, certified) - compact pos mode, delete-as-you-go.
2. fan-in merge, v2 emission mode: emits `frag.rlebwt` + `.meta`
   (byte-identical, the only pre-lite whole file) + per-run-band PIECES
   `{char u8, len u32, head SA packed, tail SA packed}` (the ONE canonical
   packed copy of each SA column, w = bits(n+9)); NO `.ssa`, NO `.ssa_t`,
   NO `pftext`, NO `pfck`. Pieces are rename-published per band, flow-
   controlled against the consumer, deleted after the container publishes.
3. fused finish (new `pile_finish`), pipelined per band with the emission:
   endpoints-equivalent repair stream (certified frames only; non-certified
   frames refuse loud) -> per-band slim LCE over the corpus adapter (PFCK
   built from the corpus by one backward read) -> sequential sweep state
   machine -> witnesses to RANGE-PARTITIONED buckets -> EF -> the lite
   container (m1 written streaming with upfront Huffman codes, m5 EF, m11
   sparse tails). NO agg/ssa_t/head_sa file ever exists.
4. rlebwt deleted after the container publishes (it survives pre-lite for
   the G1/G2 byte-bridge comparisons).

Peak arithmetic at 1GB (v2 terms): rlebwt 1.47n + container ~0.46n +
pfck scratch 0.36n + piece windows <= ~2.5n over the scratch tree; the
corpus (1n) is outside the tree and reported whole-box alongside.

## Gate order

G1 100MB (chi=29716349 exact; bridge: sorted witness stream == banked
`mergperf-100m-bank/finish/frag.sA` byte-identical, rlebwt byte-identical,
ri4 equivalence reconstructed gate-side from pieces, NO agg/ssa_t/head_sa in
the tree - asserted). G2 1GB flat (chi=283296933, peak <= 2.5n, end tree =
rlebwt + product + logs). G3 1GB fan-in k=4 (product shape-independence).
G4 1GB container write path (rlebwt deleted after; container byte-identical
across shapes). G5 plan-first v2 (per-term table + DISC verdict + pile
dry-run FEASIBLE). `PILE_PLAN_V2.md` lands in this directory at the end.

## Honest findings to carry into PILE_PLAN_V2.md

* The mission's ~2.5n peak target is an OUTPUT-side target (the survey's
  accounting; the 30.9n/40TB pile DISC_FAIL was output-bound). The merge's
  own transient (m2 text 1n + WM 8n + bwt ~1n, measured ~12.7n at 1GB) is a
  separate per-term with its own provenance; at pile scale it needs the
  `--stripe` accounting the plan model already carries (reader-level
  striping is banked as lane item 3). The v2 per-term table prints it
  explicitly rather than hiding it in a constant.
* The finish's PFCK checkpoint sidecar (R bytes, 0.36n at 1GB / ~0.48 TB at
  pile) is a scratch term the mission's peak model did not name; it is
  journaled, deleted after the finish, and included in the table.

## M3 - the fused banded finish at 100MB: GREEN (2026-10-11)

`pile_finish` (bit6/pile_finish.cpp) completes the v2 pipeline end-to-end at
100MB, run CONCURRENTLY with the merge (the real pipeline shape: the finish
opens the merge's WM scratch during the pair phase - the WM is deleted at
merge end, so a late finish fails loud on the poll timeout by design).

The open seam-class bug is root-caused as TWO independent defects, both fixed:

1. **The WM is row order, not sorted.** `wm.bin` is the merged SA in ROW
   order (`out += nb` in the emission; consumed as `wm.at(t-10)`), so the
   rescued code's `wm_lower_bound` binary search could never work (it was
   never exercised: the smoke test died waiting for the WM). The seam
   machinery now computes rows with NO search over the WM at all:
   discovery = the occurrence chain of P_d = T[n-d..n) straight from the
   corpus; ONE sequential WM scan collects lo = row(first_d) (v1's
   leftmost-row invariant check(lo==row) makes this exact); before/after and
   the class SA sets are single WM preads (SA[lo-1] / SA[hi] / SA[lo..hi));
   and the WM-read SA set must equal the corpus chain set element-for-
   element (two independent sources; divergence is fatal).
2. **The repair stream's final flush never reached the sweep.** The last
   2 runs (the coalesced tail runs held back for char-merge parity,
   emitted by finishStream after the band loop) were never LCE'd or
   advanced - the production sweep's last events live exactly there. The
   per-band LCE block is refactored into processBuffer() and flushed once
   more after finishStream(). (This was the -2 chi: 29716347 vs 29716349,
   with the witness stream byte-identical up to the last two witnesses.)
   Also fixed: the m11 sparse index was off by one (emitOne pre-increments
   rNorm; sparse tails live at run indices 0, 1024, ...).

Evidence (all in this directory; m3-100m/finish.log, m3-100m/merge.log):

* chi = 29716349 EXACT (banked mergperf-100m XSA_BUILD_DONE constant).
* Sorted-witness bridge `frag.sA` byte-identical to
  `mergperf-100m-bank/finish/frag.sA` (29,716,349 x u64).
* Seam classes byte-identical to the v1 chain's SLIM_SEAM_CLASS2 dump
  (4 classes, 16573 rows, 11658 rebuilt runs, discovery_steps=5,
  max_seam_lce=1983, predecessors_before_zero=7268; v1 dump regenerated
  from the instrumented v1 endpoints under dbg-v1 and cmp'd - see
  v1-seam-classes.txt / v2-seam-classes.txt).
* The normalized run stream is byte-identical to the bank's v1 ri4:
  chars, lens AND packed tails (n-1-tail convention) all cmp clean; the
  per-run LCE columns (bridge scratch, deleted on success) matched the
  bank's frag.agg topLCP/interiorMin for all 38,647,515 runs after fix 2.
* rlebwt + meta byte-identical to the bank (kept pre-lite per G1).
* Container lite published: 48,768,685 bytes (0.49n) with members
  {m1 runs, m4 anchors, m5 chi EF, m7 remap, m10 LF-derived, m11 sparse
  1/1024 tails}; m1 C table == bank ri4 C; m11 sparse tails == bank-derived.
* NO agg/ssa_t/head_sa/ssa/pftext/pfck file exists anywhere in the tree.
* The m1 replay re-derives the normalized runs from rlebwt+classes
  sample-free and cross-checks per-char totals/runFreq (passed).

Pipeline shape notes: the finish waits for the merge's `runcols.idx`,
builds the PFCK from the corpus, opens the WM as soon as its size hits
8n (the merge's last shard flush is the only writer that extends the file
to full size, so size==8n implies complete), then seam classes -> sweep
(pieces flow-controlled; consumed sentinel frees the merge's window).
The m1 replay barrier decode-counts the rlebwt until all R runs/N rows
are visible (the body is the emission's final segment concatenation).

## G2 - 1GB flat k=10: GREEN (2026-10-11)

Concurrent pipeline (merge + finish, window=64 bands, the real flow-controlled
shape), gate runs alone, caps (timeout 21600 + ulimit -v 512GB) on both,
bridge dir outside the measured tree, --wit-ram-budget 32GB (no witness
spills: chi*8 = 2.27GB < budget; RAM is not the mission constraint).

* chi = 283296933 EXACT (the banked 1GB constant; in-process assert).
* rlebwt + meta byte-identical to banked mergperf-1b-B (10-chunk flat
  k=10 merge, gate-1b-flat10-d chunks).
* The corpus-built PFCK byte-identical to the bank's --emit-pf sidecar
  (count=45454576, tau=22) - the M1 parity gate at 1GB.
* Witness stream (bridge, emission order) byte-identical to
  mergperf-1b-B/finish/frag.sA (283,296,933 x u64, 2.27 GB cmp clean);
  WITNESS_SAMPLE_CHECK 1025/1025.
* Seam parity vs the bank's v1 endpoints log: classes=30 rows=213516
  discovery_steps=31 limit=368036 phi_samples=368036610 max_seam_lce=97059
  predecessors_before_zero=73250 - every printed constant identical.
* ENDPOINT_STREAM_PASS r=368036609 == the bank's normalized r.
* Container lite: 467,814,822 bytes (0.468n), members m1/m4/m5/m7/m10/m11.
* SCRATCH-TREE PEAK (excluding the merge's own .xsc transient, which is a
  separate merge-side per-term): 1,974,887,564 bytes = 1.975n <= 2.5n
  PASS (the charter's ~2.0n expectation). The merge transient peaked at
  14.46n (m2 2n + WM 8n + bwt/hash ~4n) and is reported, not hidden.
* End tree after gate cleanup: product + logs only (rlebwt/pfck deleted
  post-gate per the rules). NO agg/ssa_t/head_sa/ssa/pftext ever exists.

Measured 1GB walls (for PILE_PLAN_V2.md; the pfck-corpus phase includes the
~32-min runcols-idx poll while the merge walks): pfck build 7.3s (linear),
seam-classes 202s (WM scan 8n + 30-class rebuild), sweep 4306s (repair
1276s + LCE 967s + witnesses), m5-ef 27s, m1-runs 32s, publish 0.2s.
Finish cumulative 6521s; merge walk ~32 min (48 threads).

## G3 + G4 - 1GB fan-in k=4 + the lite-container write path: GREEN (2026-10-11)

G3 (levels 10->3->1 over the same deterministic 10-chunk set, mwork
intermediates, window=64) and G4 (the default container-write path) land
together in one run:

* chi = 283296933 EXACT again (shape independence); classes=30
  rows=213516 steps=31 max_seam_lce=97059 pred=73250 - same as flat.
* ENDPOINT_STREAM_PASS r=368036609 == the bank's normalized r.
* THE PRODUCT IS SHAPE-INDEPENDENT: the lite container is byte-identical
  across the flat k=10 (G2) and fan-in k=4 (G3) shapes - 467,814,822
  bytes, members m1/m4/m5/m7/m10/m11, every member count/bytes pair equal.
* The emission-order witness stream (bridge) is byte-identical to the
  bank's frag.sA for the fan-in shape too; WITNESS_SAMPLE_CHECK 1025/1025.
* G4: run WITHOUT --keep-rlebwt - PILE_FINISH_RLEBWT_DELETED in the log;
  the end tree holds product + logs only (rlebwt, meta, pfck, pieces,
  idx, consumed sentinel and the mwork intermediates are all gone; the
  merge's .xsc cleaned itself). NO agg/ssa_t/head_sa/ssa/pftext/pfck file
  ever exists in either shape's tree.
* Fan-in walls: the whole fan-in tree merge (3 levels) + finish ran
  concurrent end-to-end in 6349s (finish cumulative), comparable to flat
  despite 3 merge levels - the emission window paces to the consumer.
