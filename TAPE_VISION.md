# TAPE_VISION.md — the machine we are pursuing (the operator's design, consolidated)

Adopted 2026-11-20. This document supersedes the amendment history in
TAPE_BINS.md as the statement of intent. The machine:

```
STREAM   one single-threaded pass over the corpus. Local state only
         (trigger windows, a bounded recent-phrase cache). It computes
         the parse on the fly — the parse is a property of the stream,
         never a stored text. It seeds every downstream structure as it
         goes. The corpus is read here, and its bytes never copied.

SORT     the phrase KEYS, once — the only byte-order sort in the system.
         Dedupe is its byproduct, and so is the CENSUS: for every distinct
         key, its occurrence count. No sampling. No intern table in RAM:
         dedupe and census both live in this sort, in pieces.

BIN      the census draws the bins: balanced ranges over the sorted keys,
         boundaries by counts. Never first-byte. Never sampled. Bins are
         defended by measurement.

SORT     each bin's chunk, IN PARSE SPACE — integer IDs, the easy sort
         (ID order is byte order by construction). One bin in RAM at a
         time; the workspace dial sizes it; any reasonably-sized system
         runs any corpus.

CONCAT   bin outputs in key order. There is no merge step. The output is
         the container.
```

## The recursion — the parse of the parse

The parse is a text (of IDs, in byte order). Repeated BLOCKS in the corpus
are repeated ID-sequences in the parse; to find them, PARSE THE PARSE —
the same streaming trigger over ID k-mers. A block that occurs M times
becomes one level-2 entry with M integer occurrences. Condensation
compounds per level; each level divides by the phrase length, so the
onion shrinks fast (77B rows -> a few GB -> a few hundred MB).

THE CENSUS DECIDES THE LEVEL, once per corpus, by measurement — the
level-2 streaming parse over level 1 costs minutes and IS its own
feasibility study: its dictionary size and phrase lengths are the yield.
If the blocks are there, the SA is built at that level and the induction
expands back down (level N -> level 1 -> corpus). The blade never decides
anything; we choose the level of the onion with a census.

## The laws (all inherited, all non-negotiable)

- ONE MODE. Every phase's resident set = the workspace dial + O(1)
  buffers, at every scale. No phase holds a corpus- or
  dictionary-proportional structure in RAM — the parse seeds, the sorts
  consume. Proof: the same container, byte-identical, at dial 512 MB /
  2 GB / roomy, with per-phase peak RSS asserted <= dial in every
  receipt.
- NO STORED BYTES. The corpus is the only copy of the text. Every file
  the machine writes is positions, IDs, and references (8-16 B each).
  No expansion: peak disc ~3.5 TB at the pile, phase inputs deleted as
  outputs complete, the statvfs governor refuses with numbers.
- NO-CROSS. Documents are independent parse units at every level;
  comparisons end at document ends.
- THE TRIGGER. One-sided (strictly smaller than the w preceding k-mers,
  suppressed in each phrase's first w positions) — provably prefix-free;
  the walk remains only as the doc-end residual's safety net, journaled.
- NO DECISIONS IN THE BLADE. Uniform strokes; level selection is a
  one-time corpus-level census measurement, never a per-row branch.
- NO SIGNALS. Files and exit codes. Refusals die with numbers.
- THE PROOF STRUCTURE. The brute oracle at fixture scale; byte-identity
  against the trusted machine at 3 GB; the invariance SHAs (band-size
  and dial-invariant outputs); SS_DONE or VOID; denominators on every
  number.

## The ladder

1. THE 3 GB PROOF under the one-mode law: streaming parse seeds, census
   bins, parse-space sorts, concat emit — fast (minutes per phase),
   efficient (rchar ~5.7x), controlled (RSS = dial, asserted), and
   byte-identical across three dial settings.
2. THE LEVEL-2 CENSUS at 3 GB: parse the parse, measure the yield
   (dictionary size, phrase lengths), decide the level ONCE by number.
3. THE 100 GB HEAD: the same machine, 10x scale, the disc-resident paths
   carrying real weight. Not a rehearsal for a special mode — the same
   mode, watched.
4. THE PILE: 1.31 TB, the same machine with a bigger input. Days.
   Launch on G3's measured projection.
