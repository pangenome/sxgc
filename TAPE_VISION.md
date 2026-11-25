# TAPE_VISION.md — the machine we are pursuing (the operator's design, consolidated)

Adopted 2026-11-20. This is the **target algorithm for `xsa build`**, not a description of everything that currently runs. [Amendments 6 and 7b](TAPE_BINS.md#amendment-6--the-sort-sorts-the-parse-the-radix-system-completed-2026-11-19) record the operator's distinction: **the parse is the tape**. One public builder; the workspace and thread settings change the schedule, not what gets sorted.

1. **Parse and name.** One sequential parse of the immutable corpus. Keep dictionary phrases by reference. Deduplicate, assign IDs in lexical order, and count occurrences in bounded pieces. The new tape `P` is those integer IDs **in original text order**. Storing an ID tape is intended; storing another copy of the raw text is not.
2. **Sort suffixes of `P`.** These are suffixes starting at phrase boundaries, not one sort record per byte of the corpus. When another level is warranted, its input is this ID tape; sort at the smaller level and induce back down.
3. **Expand by prefix-free representative.** Under the PFP convention, each byte suffix has a representative ending at the next trigger including the overlap (handle END explicitly). Distinct eligible representatives are prefix-free, so their dictionary order is their final group order. Within an equal group, use the already-sorted *next* parse suffixes. Establish or reuse representative order **as dictionary work**, not by comparing text suffixes for every occurrence. This is the [PFP ordering guarantee](https://doi.org/10.4230/LIPIcs.WABI.2018.2); a changed trigger or forced boundary needs its own justification, not just the name “PFP”.
4. **Stream in final order.** Use exact counts, not samples, to size bounded key ranges. They concatenate in order; no global suffix merge. Carry adjacent byte LCP across seams and stream `(original byte position, byte LCP, predecessor)` including virtual END to the boundary-aware v10 writer. Use the authoritative document map. No full byte SA/LCP spool.

## How to tell whether code implements this design

A matching index checks **the result**, not **the algorithm**. In qualification, identify the actual sort calls and their input units: dictionary keys/representatives, parse positions at each level, or raw text positions. Account separately for dictionary-offset visits and final emitted rows. Change the workspace on the same small input to force more output ranges: more ranges must **not** trigger another full corpus or full dictionary-offset walk per range. Check the emitted order, ties, seam LCPs and END against a small independent oracle. These are development checks, not extra passes inside an ordinary build.

### Evidence at 2026-11-25 — not a menu of engines

| Existing work | What it establishes | What it does not establish |
|---|---|---|
| [Raw tape rehearsal](RESULTS_REHEARSAL_100GB.md) | `scatter_sort` and the old finish produced a 100 GB v10 result. | Its sort input was raw suffix positions, not `P`. It is a baseline. |
| [Recursive parse prototype](RESULTS_TAPE_READER.md#10-gb-recursive-rust-full-reference-identity) | Integer levels and induction produced the reference 10 GB **v8** product. | Not an installed v10 path or an all-phase workspace proof. |
| [Overlapping PFP fixture](OVERLAP_PFP_PROBE.md) | Dictionary representative order + parse continuation rank reproduced byte SA/LCP on fixtures. | Not evidence that the dictionary work fits at scale. |
| [Rolling `build-stream`](docs/build-stream.md) and [receipts](gates/receipts/build-stream/) | Ranks a parse and feeds a direct v10 writer; complete through 10 GB. | Its emitter revisits the full offset set per posting batch. A working writer does not qualify that producer. |

No receipt above establishes the entire target. Preserve these implementations and results; reuse a component on the strength of its own contract, not its name.

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

## Original qualification milestones (targets, not completed results or launch instructions)

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
