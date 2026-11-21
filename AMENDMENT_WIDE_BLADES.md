# AMENDMENT W (WIDE BLADES) — 2026-11-21, spec before dispatch

Status: FROZEN. This is an amendment to TAPE_VISION.md's final shape.
It changes NO law of the machine. One mode intact, dial intact, no
stored bytes, no signals, no decisions in the blade. Every change is a
scheduler/kernel-width change inside the existing bands.

## Motivation (measured, Rung 3 live, 10 GB producer)

  parse                130 s      (fine — the trigger walk is already cheap)
  dict sort + SA + pairs ~2 h     (sort kernels running at a few cores)
  emit                  in flight (the_concat_walk at 406% of 256 cores)
  finish                overlapped (ONE core serializing member streams)

The machine's wall time lives in sorts and copies running 1–4 cores wide
on a 256-core box. The Sawzall law already made every band uniform and
decision-free — bands are embarrassingly parallel units of work. We were
simply running them narrow.

## W1 — Fan the bands

Every band-shaped sort phase (dict sort, parse SA, pairs) runs its bands
concurrently across the existing --threads pool. Per-band work content is
unchanged; band boundaries are unchanged; output order is the existing
key order (no merge added — band order is commit order, as A3 requires).

## W2 — The per-band SA kernel becomes libsais

The single-threaded SA construction inside the band blade is replaced by
libsais (OpenMP build) as the kernel. This is a library swap INSIDE the
blade, not a new mode: banded machinery, dial, DIAL_LAW assertion, and
byte output all unchanged. libsais' 32-bit index ceiling is exactly what
the level-2 recursion answers at pile scale — the level decision still
comes from the census number, unchanged.

## W3 — Emit pool goes wide

The concat walk's unit pool is governed by --threads (currently 4).
Per-row work remains a copy; no sorting appears (rule=the_concat_walk;
no_sorting receipt unchanged).

## W4 — Finish: parallel member streams

m1..m9 are independent members; the finish streams them concurrently
while remaining overlapped with emit. Container format v10 forward-only
is untouched; member order within the container is unchanged.

## W5 — Emit carries the LCP (the concat law, completed)

The emit's row ORDER is precomputed (the lists carry it), but each
row's LCP vs its predecessor is recomputed live by walking corpus
bytes (suf_cmp_lcp), unbounded on repetitive text — the measured emit
wall. Fix: carry the LCP like the order. The parse LCP array (already
computed in the LCP phase) bounds every row's byte-work:

  - leading phrase IDs equal: lcp = phrase_len + parse_lcp lookup — O(1)
  - leading phrase IDs differ: byte-compare inside the two phrases,
    bounded by min(len(P1), len(P2)) — never past the phrase boundary
  - induced/unaligned rows: same induction locally within the phrase

Emit per-row work becomes a bounded copy — every fact precomputed and
pre-ordered, as the concat law demands. The residual byte-compare may
use word-at-a-time (u64 xor + ctz) instead of the byte/pair loop as a
stacking kernel win; g_text_walk keeps journaling walked bytes and
must show the collapse (receipt: text_walk before/after at sandbox).

## W6 — Parse parallelism: DEFERRED

The parse is the cheapest phase (130 s at 10 GB). Only if a profile at
100 GB says otherwise, parallelize by document boundary (the no-cross
law gives free split points). No work under this amendment.

## Verification — small scale FIRST, speed is a receipt

   Emit receipts must additionally journal text_walk totals and per-phase
walk_s/assemble_s before and after W5 (the LCP carry), so the collapse is
a number, not a claim.

1. Sandbox (100 MB), same dial arms (16 MB / 512 MB / wide):
   - byte-identity: SHA must equal the certified 6325c843…f341, all arms
   - speed receipt: per-phase wall table, old vs new machine, journaled
2. G0 fixture suite: 10/10 oracle-exact, band-invariance SHA 31a764b6…
3. 1 GB and 3 GB: byte-identity vs the certified climb containers
4. 10 GB: byte-identity vs the Rung 3 container (the anchor landing now)
5. DIAL_LAW armed throughout; anon peaks under the dial at every arm;
   no assertion may fire.

Certification ladder collapses after that: the accelerated machine goes
straight to 100 GB gated by member-level identity vs the banked rehearsal
container (Rungs 4 and 5 become one run whose phase table IS the G3
launch arithmetic and whose census number IS the pile's level decision).

## Non-goals

No algorithm changes to the parse. No new modes. No merge machinery.
No stored bytes. No timeouts. The 4,449-line rewrite is still queued
behind the pile and out of scope here.
