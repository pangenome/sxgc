# TAPE_BINS.md — the per-bin machine (v2, supersedes the pull-machine section of TAPE_READER.md)

Operator's design, frozen 2026-11-17. THE LAW: every row's bytes are read at most
once per expansion need; no central reader mediates; no match crosses a document
boundary (separator framing, verified at open); no signals ever (files + exit codes);
a run without SS_DONE is VOID; our guards fire before the OS's (memory and disk).

## The machine

Stage 0 — RADIX SCATTER (shared, exactly once):
  One sequential pass over the corpus (the existing scatter discipline: blocks with
  boundary overlap). Every suffix position is bucketed into one of 256 first-byte
  bins; each bin receives (position, K-byte key) rows. A bin's position list is
  ascending by construction. rchar = corpus, exactly once.

Stage 1 — PER-BIN SORT (parallel, in memory):
  Each bin sorts with the PROVEN leaf sort (8-bit MSD, virtual END = bucket 0,
  run-length digit; order-identity green at 44M rows vs brute force). Rows that tie
  beyond K form tie classes with per-row state: position (u64) + cmp_len (u32,
  bytes compared so far). Ordinary bins finish here and are never touched again.

Stage 2 — EXPANSION ROUNDS (the bin's OWN readhead):
  A bin with unresolved tie classes walks ITS OWN ascending position list and reads
  [pos + cmp_len, pos + cmp_len + delta) with gap-merged ascending preads. Only the
  wanted bytes are billed (pread bills bytes read, not bytes skipped). Compare,
  partition, and repeat for survivors only; cmp_len grows only for rows still tied.
  delta schedule: start small (e.g. 256 B), grow bounded (e.g. doubling, capped).
  Total window bytes == sum over rows of their true divergence depth — most rows
  are cheap; the expensive ones are exactly the fat classes of Stage 3.

Stage 3 — SUB-PFP (per fat class, the compression):
  TRIGGER (measured, not hoped — ONE PRINCIPLE, two clauses: route a class to
  the parse WHICHEVER WAY it is expensive). The parse is the compression for
  classes the byte expansion cannot afford — expensive by WIDTH or by DEPTH:
  - WIDTH: a class whose projected expansion cost exceeds its bin budget —
    projected from the class's own census (rows still tied after round r
    x current cmp_len growth), OR any class still unresolved when its
    rows x cmp_len passes the budget line.
  - DEPTH: a class whose cmp_len exceeds the parse's one-shot capture cost
    (phrase_budget x (k+w) x 2 + 4096 bytes) MUST parse — at that depth the
    expansion has already paid more than the parse's entire capture. The
    3 GB cold receipt that forced this clause: tie classes of a few rows
    diverging at depths up to 262 KB — rows x cmp_len = ~26 MB, far under
    any per-class budget, so the width clauses never fired; the expansion
    paid rows x 2 x depth for each of hundreds of thousands of them
    (16.8 GB on 35/256 bins) while the parse costs rows x ~6 KB once.
  The bin announces the parse and:
  - parses the class's continuations starting at the radix prefix (per-class PFP:
    dictionary of phrases from the two-sided trigger rule, per-row keys
    (phrase id, intra-phrase offset, following phrase sequence));
  - the deep shared bytes exist ONCE in the dictionary, not per row;
  - sorts in phrase space with the MODALITY-SLIDE COMPARATOR (below);
  - the bin's readhead fetches the class's shared region once.

Stage 4 — CONCATENATION:
  Bins concatenate in key order. The concatenation IS the merge. No global merge
  step exists.

## The modality slide (exactness without prefix-freeness)

DEFAULT — phrase space: compare phrase-id sequences; ids intern exact byte strings,
so unambiguous phrase order IS byte order. No I/O.

SLIDE — at any ambiguity (a phrase pair that could be prefix-related, i.e. the
overlap case where one trigger phrase sits inside another): walk the two rows'
ACTUAL BYTES from their positions until the comparison resolves, then RETURN to
phrase space. Per-comparison, bounded by the ambiguity span, journaled as
prefix_relations (health metric, never a claimed invariant — prefix-freeness was
DISPROVED for the two-sided trigger rule; the machine must not need it).

REVERSE SLIDE — if a class's dictionary grows past that class's pure byte-expansion
cost (pathological parse), abandon the parse for that class and continue Stage 2
byte expansion. Journaled (parse_abandoned, with both costs). No class can trap the
machine in either modality.

Exactness rests on: (a) the fallback reads real bytes (exact by definition),
(b) the brute-force SA oracle at every gate. Structural bug = hard die: a proper
phrase shorter than its trigger k-mer.

## The readheads

Every bin reads for itself. One position cursor per bin, ascending, gap-merged
preads on the shared corpus fd. Serial within a bin, parallel across bins — NVMe's
best case; the box has 256 cores. NO request queue, NO matcher/automaton, NO
admission, NO cohorts, NO ledger, NO central tape. If an implementation of this
design reintroduces a request queue or per-prefix automaton, it is wrong — re-read
this file.

## Memory

Per bin: its chunk + keys + active expansion buffers + (active parse dictionary).
The bin budget caps each term. A class over budget parses (Stage 3) or reverse-slides
to bytes; a BIN over budget is a plan error — die with numbers (bin id, rows, bytes).
Global residency = sum of bins, planned to fit. In-process guard refuses with numbers
before the OS OOM killer fires — non-negotiable (two box OOMs happened this week).
Post-run live == 0 asserted.

## Termination

Each expansion round resolves rows or grows cmp_len (bounded by document length);
each parse terminates by the widening argument (rounds x8 then LCE residual — the
committed mechanism). Every loop carries a step counter that dies with numbers
(position, class, round, steps) — no silent spins ever (a 90-minute silent park and
a 1h18m spin both happened; both must be structurally impossible).

## Gates (unchanged — the honest ones)

G0  fixture gate: 7 fixtures + the parse-collision adversarial case (16 KiB, logic
    size only), each vs the brute-force SA oracle, 0 mismatches, prefix_relations
    journaled.
G1  P1 COLD: 3 GB corpus, fadvise DONTNEED per arm, both arms built from THIS branch
    (per-bin binary vs the tail-patched scatter comparison binary), identical flags.
    ACCEPT: rchar/corpus <= 3.0, SS_DONE both arms, denominators printed
    (rchar, read_bytes, syscr, rows, wall). VOID without SS_DONE. On FAIL: report
    per-class demand accounting (which classes consumed the bytes, rows x depth),
    STOP — no knob tuning.
G2  P2 identity: 3 GB container byte-identical between the arms; then the 100 GB
    head (/mnt/nvme2n1/erikg/pile-head100.txt + /mnt/nvme2n1/erikg/pilemap100)
    byte-identical. First differing unit => STOP and report.
G3  pile-wall projection: pass-1 sweep time + parallel sort rate x 13.08e9 rows +
    measured window/parse demand => finish estimate, arithmetic shown.

## Discipline

No wall-clock timeouts on measurements (in-process counters; truncated = VOID).
No signals from the product (stop.requested / geometry.ctl / exit codes only).
Commit per gate, push after every gate, verify origin == HEAD && ahead == 0 && dirty == 0.
Long runs detached (setsid nohup). Scratch under /mnt/nvme2n1/erikg/TAPEREADER;
NEVER under /mnt/nvme2n1/erikg/PILE. -j 24. Report at gate boundaries with denominators.

## What this deletes

The central tape's request queue, matcher, ledger/credit admission, cohort
restructure, and resumable sweeps — the entire mediation stack. The proven parts are
kept: scatter pass, leaf sort, parse layer, modality-slide comparator, oracle gates.
