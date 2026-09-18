# The Bit Ladder — serial execution plan (one bit green before the next starts)

Constraints of record: **AGC as only source; minimal RAM; near-zero scratch;
no flat text; no interim full-collection baseline.**

| Bit | Deliverable | Gate (must be green) | Status |
|---|---|---|---|
| 1 | Lean: Defs + one-pass scan + executable checks | 10/10 random texts (done) | ✅ |
| 1b | **Exhaustive** verification: all texts over {1,2}, |T| ≤ 8 — covering AND minimality | 0 failures, exhaustive | 🔄 |
| 2 | Rust `scan-rs`: consumes (c,lcp,sa) triples, emits χ-set; fork patch `--dump-triples` | **GREEN**: tiny (baa) + yeast235 **exact vs C++ pfp route** (7,501,037 byte-identical); cross-route vs one-pass differs ONLY by tie-breaking (611,520 positions, 92% overlap — smallest sets non-unique, Bit-1-established) + one-pass ±1 terminator convention | ✅ |
| 3 | FIFO pscan: AGC stream → dict/parse (no flat) | dict/parse byte-equal vs flat-mode | ⏳ |
| 4 | PFP-aux emission in streamed scan (lens/lcs/alph) | differential vs `one-pass-build-index` components | ⏳ |
| 5 | `-o agc` oracle (ragc FFI + LRU) | byte-verify vs flat text; 0 oracle disk | ⏳ |
| 6 | full-466 run (AGC-native, streamed) | memory profile + χ + verified MEMs; no flat text ever | ⏳ |

Deferred (explicitly, with specs): toehold GAF (Phase 5), tag arrays (Phase 6),
χ_tag (RESEARCH.md). Bit 1b's general theorems (`sorry`s) stay as research-track
items; exhaustive domain checks are the Bit-1b gate.

## Differential-gate convention (from Bit 2)

Cross-route set comparison is NEVER exact (smallest suffixient sets are
non-unique — tie-breaking). Gates: (a) **same-route exact match** (scan-rs vs
the C++ route that produced the stream), (b) χ equality, (c) covering —
machine-gated in Lean (511 exhaustive). one-pass additionally emits the
terminator-run candidate (±1 entry vs Def. 9's alphabet-only extensions).

## Stream-convention contract (discovered in Bit 1 — binding for Bits 2/4)

The one-pass consumes `(bwt, lcp, sa)` triples of **reverse(input) + 0-sentinel**
in SA order with **0-based sdsl SA**; the scan emits `N - sa` (1-based text
positions); `eval` iterates chars **1..σ** (sentinel never emitted); chars are
remapped to 1..σ by first appearance in the reversed text (scan output is
invariant to relabeling — per-char state machine). The pfp route streams the
same colex-order triples via the PFP iterator.
