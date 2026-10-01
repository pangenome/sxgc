# Cross-boundary LCE: design and implementation statement lock

**Lock:** implement a budgeted, short-prefix-first fingerprint comparison service,
with exact shared-phrase shortcuts where their alignment is certified. Recompute
cross-boundary order and explicitly represent/refine intervals that split. Do not
implement a stable row permutation or whole-run interleave. This is a comparison
and refinement design, **not a proved O(runs) merge algorithm**. The measurement
supports fingerprints as the general comparison path; it does not support shared
dictionary membership as a shortcut for most comparisons on this fragment.

The deliverable is `measurements.jsonl`, `RESULTS.md`, `cost-model.json`, reproducible
measurement/test code, and this design. No production merge was added. The
experiment measures all 15 adjacent chunk pairs of the retained 1,082,130,213-byte
Pile fragment. It does not measure the 466 pangenome or the full Pile.

## What was actually compared

A's ordered candidates are its cyclic-BWT run **heads and tails**, with one
endpoint for singleton runs. B queries are uniformly sampled run heads, with
replacement and a recorded seed. Each B head is binary-searched into A's local
cyclic endpoint order. We measure the strict predecessor and first greater-or-equal
successor, their individual LCEs, their maximum, and their summed comparison cost.
A head equal to several A rotations is paired with the first equal endpoint.
The equality test uses infinite local periodic contexts, capped at
`|A| + |B| - gcd(|A|, |B|)`; equality at that bound is recorded as periodic equality,
not a finite LCE. No production sample reached periodic equality.

This is the nearest candidate **in the stored endpoint set**, not necessarily the
nearest row of A's full BWT. The largest LCE to the endpoint set is a lower bound
on the largest LCE to all rows. Sampling B heads also says nothing about the
cost of arbitrary interior B rows. Neither fact is hidden in the extrapolation.

A local cycle's continuation changes when chunks are concatenated. Old row order
and old whole-run interleave are already falsified assumptions. The diagnostic
binary search is valid only in local cyclic context. It must not be copied into a
merger using new concatenated contexts until that search structure is reordered
or its unaffected order has been proved. A small sampled seam incidence is not
permission to ignore the affected rows: omitted rare rows can determine the
entire order of a long equal-prefix class.

## Measurement mechanics and exactness

`measure.cpp` opens the corpus once, streams it sequentially inside the executable,
applies the retained parser remap, and builds two dense prefix fingerprints.
Each chunk, dictionary and parse file is also read once. It creates no source
substring files and never reconstructs text through LF. The source, reference
artifacts, and all retained chunks are read-only.

This is deliberately a **measurement-only dense index**: O(N) setup and memory,
plus O(R) endpoint storage. It is not a compressed merge implementation and must
not be extrapolated as one. Prefix hashes alone use 16(N+1) bytes; the table of
powers uses another 16(2 max-chunk-length+1) bytes. Source symbols, endpoints and
parse metadata are additional. The actual `/usr/bin/time -v` journal supplies
peak RSS and total wall, separately from per-boundary query time.

Each LCE has a 16-symbol fast path, exponential hash probes, then binary search
inside the first unequal interval. Two fixed odd polynomial bases modulo 2^64
are used as proposals, not as an exactness theorem. The pilot allowed at most
4,096 directly checked prefix symbols; the expanded run allows 131,072 plus one
mismatch byte. Both impose <=128 hash equality probes/query and <=10^10 total
charged work units. A work unit is one quick/direct symbol comparison or one
hash equality probe. The latter involves a constant number of prefix accesses
for these similarly sized chunks. General unequal lengths use logarithmically
many hash concatenations. Preprocessing is reported separately, not charged to
this query budget. No cap expands to the corpus length; no LF/full-text fallback
exists. A budget overrun exits with an error.

If the proposed prefix fits the verification cap, every proposed equal symbol
and the first unequal symbol are checked, making that particular answer exact
even if a hash collision occurred elsewhere in the search. Any larger proposal
is explicitly counted as `not_fully_verified_queries`; such observations must be
treated as uncertified. `RESULTS.md` and `summary.log` report the final counter.
Witness rows retain each boundary's record-breaking candidate positions.

## (a) Per-run comparisons with prefix doubling / fingerprints

Inputs must provide random access or compressed navigation to the **new** cyclic
context of each frontier endpoint. A comparison proceeds as follows:

1. Compare a bounded short prefix, stopping at the first unequal symbol.
2. If unresolved, test prefix lengths geometrically, then bisect the final range.
   A substring fingerprint is a difference of suffix/prefix fingerprints; a seam
   is handled by composing a bounded number of segments. Charge every probe.
3. Certify the equal prefix and first unequal symbol. Reuse an exact shared-phrase
   or certified block identity if available; otherwise charge direct verification
   against explicit per-query and total limits. Refuse on exhaustion.
4. Feed the resulting order and LCE to an interval-refinement layer. Only emit an
   entire interval when all its rows are certified to remain contiguous in the
   new order and to share the emitted preceding symbol. Otherwise split/refine
   and charge those operations. Merge adjacent equal emitted symbols afterward.

SLIM_FP is the reference for this discipline, particularly the distinction
between probes, sampled-value reconstruction work and direct verification.
Its current verifier still scans the proposed equal byte/phrase prefix. Merely
using hash-based LCE does **not** remove that cost or establish logarithmic exact
LCE. Its parse level can check phrase IDs instead of text bytes, which helps
long shared phrase sequences, but is still linear in the number of verified IDs.

Let R=R_A+R_B, R' be the output run count, Q the actual number of cross comparisons,
S the additional interval splits/refinements, and L_q each comparison's text LCE.
With O(1) dense hash access, probabilistic proposal work is
`O(sum_q (1 + log(1+L_q)))`. Sampled fingerprints add their reconstruction cost
(e.g. O(tau) symbol/ID reads per sampled probe). Exact byte verification adds
`Theta(sum_q (L_q+1))`; compressed certification instead adds the explicitly
measured verification work V. Refusal is not successful exact completion.

Candidate selection is a separate cost. Independent insertion into a valid
ordered frontier can require O(R_B log R_A) comparisons; the diagnostic does so.
A simple sort/refinement approach can require O((R+S) log(R+S)) comparisons and
at least Omega(R') output work. A linear sweep is only available **after** the
frontiers are sorted under the new context and whole emitted intervals are
certified. No bound S=O(R), Q=O(R), or R'=O(R) is proved here. Thus no O(R) claim
follows from fast LCE alone.

## (b) Shared dictionary anchors

Canonicalize equal phrase strings across chunks with exact content identity;
local phrase ranks or fingerprints alone are not sufficient. For a query at
positions x,y, shared phrase IDs help immediately only when the offsets within
the phrases are equal. Equal suffixes of different phrases can also help, but
need a dictionary-suffix LCE structure, not just set intersection.

After a verified prefix reaches aligned content-defined trigger boundaries,
compare canonical phrase IDs and use a fingerprint/LCE structure on the two
phrase sequences to skip equal stretches. Convert skipped IDs to text lengths
using phrase-boundary prefix sums. Compare the terminal partial phrases exactly.
Cyclic joins and independently reset parser boundaries require explicit repair;
the last-to-first phrase transition is not automatically a shared anchor.

The measured dictionaries are the sets of global-parser phrases with a full
occurrence inside a chunk. This avoids asserting the existence of independently
built chunk dictionaries (only cyclic-BWT chunk files are retained). There are
17 excluded seam/padding occurrences in the whole parse. The interior overlap
is exact under this representation; the equivalence of independently reset
parser dictionaries was not tested. Reported Jaccard, occurrence-weighted and
contribution-byte-weighted overlaps answer different questions. Candidate-aligned
hit fractions test the actual shortcut condition rather than treating dictionary
membership as proof of a usable anchor. The immediate and next-phrase hit classes
can overlap and must not be added as disjoint coverage.

Preprocessing costs at least reading/canonicalizing the relevant D dictionary
bytes and P parse IDs, plus its data structure construction. Straight scanning
of matched phrase IDs costs O(number of matched IDs), not O(1). Fingerprinting
phrase sequences gives logarithmic proposal probes but still needs exact
certification. Exact canonical doubling blocks or a grammar LCE structure are
possible follow-up directions, with their construction space/time and supported
unaligned queries proved separately; they are not implemented or claimed solved.
Materializing doubling ranks at every text position would cost O(N log N), which
is not an acceptable hidden O(R) construction.

The measured shared-dictionary overlap and aligned hit rates are too low to
justify an anchor-only design for **most** fragment comparisons. Sparse anchors
may still save a large part of the rare long-LCE work; hit frequency alone does
not settle that work-weighted benefit. The raw JSON records immediately skipped
and next-phrase bytes, but these measures overlap and are not a full multi-phrase
prototype. The design retains anchors as an optimization and records their work.

## Pile-scale interpretation and implementation gate

`cost-model.json` weights boundary means by the number of B runs. Under the
explicit assumption that a future stage has the same distribution and only the
two nearest endpoint comparisons per B run, cost is
`W = sum_Bruns (L_pred + L_succ) ≈ R * measured_mean_pair_sum`.
One comparison instead uses the recorded largest-candidate mean as a diagnostic,
not a proven algorithmic requirement. Actual search comparisons, hash probes and
measured per-head wall are reported separately; endpoint lookup is not free.

At R≈4.8e11 even a per-run allocation of eight bytes is 3.84 TB; two 64-bit
samples/run require 7.68 TB. A 200 GB machine cannot hold that frontier directly.
Production needs streaming/external partitioning, bounded resident state, and
explicit I/O and refinement counts. The fragment tool's dense hashes and cached
single-core timings cannot supply that solution. Balanced merge levels charge
all runs/comparisons at every level; a sequential growing merge sees different
neighbor populations. Neither is a free one-stage multiplication.

No observed >1M event is a sampling statement, not evidence that long tails
cannot occur. The per-boundary zero-event confidence bound is recorded; even a
few parts per million at 4.8e11 runs permits millions of expensive cases. The
466 seam maximum (18.2M symbols, from the existing seam journal) belongs to a
different corpus and comparison population, so it is a stress case rather than
an input to this fragment's mean. Heavy tails, later merge stages and text domain
shift are unmeasured uncertainties. There is no distribution-free tight mean
upper bound from these samples.

Before any implementation claims a correct merger, require tiny exhaustive
comparisons with the concatenated-text cyclic BWT including periodic equalities,
seam-order reversals, and run splitting, followed by the four retained reference
files on this fragment. Track Q, S, R', verification work, refusals, RSS and I/O.
An O(R) statement additionally needs an argument bounding order repair, splits,
comparison count, exact verification, preprocessing and output—not only favorable
LCE averages. This journal deliberately leaves that research problem open.
