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

## AMENDMENT 1 — adaptive prefix-range bins (the sampled radix plan), 2026-11-17

256 fixed first-byte bins is WRONG for real text (byte skew: space ~10% of the
pile, letters unequal, controls empty). Bins are PREFIX RANGES chosen by
sampling, with depth varying by region:

SAMPLE  one cheap pass (or reused survey stats): sample every S-th position,
        histogram suffix prefixes to depth D_max (4-6 bytes). At the pile,
        S=4096 gives ~320M samples — plenty. The wavekey-era survey machinery
        (one pass, prefix cache) is the template.

PLAN    grow a prefix trie ONLY where needed: a node splits when its sampled
        row estimate exceeds the per-bin TARGET and depth < D_max. TARGET is
        set from the wave budget: at the pile, a bin is ~5-30 GB of rows so a
        WAVE of bins fits RAM with balance across 256 cores. Sparse regions
        stay depth 1; dense regions ("the ", runs of spaces) go deeper —
        the radix pre-resolves the cheap divergence levels for free.

SCATTER per position, walk <= D_max bytes through the trie (it is cache-sized)
        to a leaf = bin id. Leaf order = lexicographic = concatenation order;
        every downstream mechanism (readheads, leaf sort, expansion rounds,
        census+parse, modality slide, guards) is UNCHANGED.

WAVES   bins stream in key order in waves packed to the RAM budget; multiple
        bins sort concurrently per wave. Peak RAM = one wave, not the corpus.
        This is the pile mode: 1.31 TB cannot be resident; the bins make it
        a sequence of independent, balanced, key-ordered problems.

G1 with fixed 256 bins (the in-flight run) is the BASELINE datapoint: it
validates the mechanisms (depth clause, guards, rchar) independent of the
partition. The adaptive plan is then implemented and G1 re-run: the partition
change is isolated, its effect measured.

## AMENDMENT 2 — the Sawzall law: one blade, every stroke, 2026-11-17

No routing decisions. No triggers. A machine that decides has decision bugs;
this session's every failure was one (automaton routing, cohort admission,
census mis-trigger). The blade is uniform:

STROKE 1  scatter (one pass, rchar = corpus once) + K-byte keys + the
          in-memory leaf sort. Rows that resolve here are done. (Not a
          decision: rows with no tie have no work to do.)
STROKE 2  EVERY surviving tie class PARSES. Uniformly. No census, no width
          clause, no depth clause, no "fat bin" trigger, no expansion
          rounds. The parse IS the tie resolution: capture the class's
          span (read exactly once), dictionary, (phrase id, offset) keys.
STROKE 3  phrase-space sort with the byte-slide comparator (the slide is
          part of the blade, not a failure mode: ambiguity -> walk bytes).
STROKE 4  concatenate in key order.

WHY THIS IS BOUNDED (and the guards mostly evaporate): the no-cross law
bounds every class's span by ONE document. Per-class memory = span (<= max
document) + rows x keys — bounded BY CONSTRUCTION, not by a guard. Uniform
parse also reads each span exactly ONCE (expansion paid ~2x by delta
doubling): the Sawzall is optimal on rchar, not merely simple.

WHAT IS DELETED: the census and its clauses, the expansion path (delta
doubling), the reverse slide (there is nothing to slide back to — bytes
live inside the comparator). The parse's internal widening rounds (x8 then
LCP residual) remain — within-stroke machinery.

WHAT REMAINS, as fuses not failure modes: step counters on every loop (a
Sawzall still has a fuse; a spin dies with numbers in seconds, not hours),
the per-class span assertion (span <= document length — an invariant of
the boundary law, violated only by a bug), the rchar backstop (journal),
post-run live == 0. These are ASSERTIONS of invariants, not decisions.

THE ACCEPTED COST: parsing the millions of small classes costs more CPU
than two expansion rounds would have. Accepted deliberately — CPU is the
abundant resource (256 cores), rchar is the scarce one, and simplicity is
the reliability strategy. The G0 adversarial shapes (single, period2, the
collision case) remain the proof of exactness.

## AMENDMENT 3 — bands: one band of workspace, ever; the output streams in key
## order with no merge, 2026-11-17

A BAND is a contiguous chunk of the FINAL OUTPUT — a key range — collected
and sorted INDEPENDENTLY. There is no all-bins position-list artifact:
position lists for the whole corpus never exist. Workspace = one band.

PER BAND:
  SCAN    one filtered corpus pass: collect positions whose suffix prefix is
          in the band's key range, capturing K-byte keys via the lookahead
          buffer as it goes. Sequential reads; only wanted positions kept.
  SORT    the leaf sort in workspace (positions + keys + parse dictionaries
          bounded by the no-cross law, per AMENDMENT 2 — every surviving
          class parses).
  EMIT    the band's chunk streams DIRECTLY to its final place in the
          container. There is no merge step and no container staging.
  FREE    everything; next band.

BAND BOUNDARIES from AMENDMENT 1's sampled histogram, equalized so one
band's workspace fits the budget (~200-400 GB at the pile). K (band count)
is printed by the plan with denominators. At fixture/3GB scale K=1 — the
G1 blade is the single-band machine; K grows only at the pile.

THE HONEST TRADE: K filtered corpus scans (K x corpus sequential reads,
a few hours at pile scale) instead of any large scratch. Sequential reads
are the cheap currency; disc pressure and complexity are the enemies.

BAND BOUNDARIES ARE COMMIT BOUNDARIES: a kill/resume restores at band
granularity (the durability law, for free). The container is final from
the first chunk onward — incremental, verifiable mid-run.

DELETED BY THIS AMENDMENT: the all-bins position lists, the delta/Rice
coder for them, the list lifecycle, and any scratch ceiling above one
band's transients.

## AMENDMENT 4 — the workspace is a knob; any reasonably-sized system runs any
## corpus, 2026-11-17

THE BAND WORKSPACE is a first-class knob (XSA_BAND_WORKSPACE_BYTES, default
a safe fraction of detected RAM, explicit override always wins). K (band
count) derives from corpus size / workspace and is PRINTED by the plan with
denominators (K, per-band rows, projected scans). The machine never assumes
the corpus, any band, or any class fits anywhere except: one tie class's
parse memory is bounded by ONE document (the no-cross law) — the hard floor
that makes any system viable.

GRACEFUL DEGRADATION: smaller workspace -> more bands -> more filtered
scans. Time scales as workspace^-1; the machine never fails for lack of
RAM, and disc never holds more than one band's transients. A 64 GB
workstation runs the pile; only the wall changes.

THE INVARIANT (a gate): the container is BAND-SIZE-INVARIANT — byte-identical
output at any workspace setting. Prove it: same fixture, workspace 64 MB vs
multi-GB, identical SHA. Band size is scheduling, never semantics.

MEMORY IS CONTROLLED, NOT ASPIRATIONAL: the ledger honors the knob exactly
(asserted, die-with-numbers on overrun — a bug, not a tuning question);
post-run live == 0.

CODICIL (2026-11-18, the operator's call on defaults): LEAN BY DEFAULT,
BIG ONLY WHEN ASKED. The workspace default is a modest ~8-16 GB (capped
at a safe fraction of detected RAM) so an ordinary build runs K in the
single-to-double digits on ordinary hardware; a big workspace is OPT-IN
via XSA_BAND_WORKSPACE_BYTES. The first default filled the box (a 3 GB
build with a 196 GB budget ran ONE band at 119 GB RSS). Both shapes are
valid measurements; the receipts must LABEL which setting each arm ran
(the SS_PLAN line's workspace_bytes is the label of record).

## AMENDMENT 5 — the global parse: bins from the PFP keys, then the sort, 2026-11-17

ONE doc-aware PFP parse of the corpus replaces the reactive per-class parses
AND the sampled census. Prior art in-repo: sA/include/pfp_iterator (the
HPRC-era machinery). The Sawzall's no-decisions property is PRESERVED and
strengthened — one parse up front, uniform, no trigger anywhere.

THE MACHINE:
PARSE (once)    a doc-aware PFP parse: each document parses independently
                (the no-cross law makes documents independent parse units);
                trigger guard at document starts; output = the dictionary of
                distinct phrases + the phrase sequence per document.
                Workspace ~1.2 N (parse array N/L x ~12 B + dictionary
                ~0.2-0.4 N); disc-resident at pile scale, sliced per band.
DICTIONARY      the distinct phrases are SORTED (byte order) — the dictionary
                is the small text; RAM-sortable at band scale; its sort may
                use the leaf-sort machinery recursively. Phrase ids <- byte
                rank. (The old sA machinery's problem, solved at small scale.)
KEYS            every suffix becomes a phrase-position row: (first phrase
                rank, intra-phrase offset, following phrase sequence).
                N/L rows with 4-byte first keys instead of N rows with 8-byte
                byte keys.
BINS            THE PARSE IS THE CENSUS: radix bins are (first-phrase-rank,
                offset) ranges — exact, balanced by measured frequency, no
                sampling, no plan trie. Amendment 3's band law applies
                unchanged: workspace knob -> K bands of phrase-key ranges,
                one band resident, filtered scans or parse-array slices.
SORT            phrase-space leaf sort within bands; ties in phrase space
                resolve by the byte-slide comparator at ambiguities only
                (bounded, journaled, oracle-gated — the counterexample law
                stands: exactness never rests on prefix-freeness).
IDENTICAL       the output container is the same artifact; the invariance
                gate (band-size-invariant SHA) applies to this machine too.

THE COMPARISON LAW: the reactive per-class machine finishes its in-flight
3 GB arm as the labeled comparison receipt (do not kill it; it runs
detached at 24 threads while this is built). Both machines face the same
gates: minis -> G0 fixtures + adversarial vs the brute oracle -> G1 cold
-> G2 identity -> G3. The numbers choose what launches the pile; no
opinions.

## AMENDMENT 6 — THE SORT SORTS THE PARSE (the radix system completed), 2026-11-19

"THE PARSE IS OUR RADIX SYSTEM" — the operator. The per-suffix phrase-streaming
band sort was a MISIMPLEMENTATION of "then the sort" and is DELETED.

THE SORT:
1. The rows to sort are the PARSE POSITIONS (N/L of them — 174M at 3 GB, not
   3e9 suffixes). Keys are INTEGER phrase-rank sequences over contiguous
   parse arrays — integer comparisons with locality, never byte-streaming
   through the dictionary.
2. ALIGNED suffixes (phrase-boundary starts): their order IS the sorted parse
   order. Sort the parse positions.
3. UNALIGNED suffixes (mid-phrase starts): INDUCED locally from the aligned
   order — each position's order follows from (its phrase's rank, intra-phrase
   offset) and the local window; O(N) induction with small, cache-local
   constants. No per-row streaming, ever. The 7.8-billion-step stream counters
   are evidence of the deleted mistake — the receipt after this amendment
   should show ZERO stream steps.
4. Bands (Amendment 3) band the PARSE SORT: key ranges of the parse order,
   same workspace knob, same invariance law.
5. EMIT unchanged: induced SA streams to the container in key order.

THE EXPECTED ARITHMETIC (from run10's receipts): parse 228 s + parse-sort
(~174M integer rows) + O(N) induction + finish — the 2h05m wall should
collapse by ~20x. G3 prices it from measurement, as always.
