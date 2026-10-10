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
