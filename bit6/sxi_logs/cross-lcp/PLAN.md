# Cross-LCP measurement journal

Started 2026-10-01 UTC at base 5cf0f1d79a4a5494387e53d9f3ba6fa5f3a749ec.
No contact_supervisor tool is exposed in this runtime (tool-name discovery checked);
no delegation, commits, staging, or external writes are used.

Retained read-only inputs:
- /home/erikg/sxgc/vendor/chunk-merge-v3/chunks/chunk-{0..15}.crle
- /home/erikg/sxgc/vendor/chunk-merge-v3/reference/parse.{dict,parse,remap}
- /home/erikg/sxgc-piletest/pile-frag.txt

The local vendor copy is absent; the main checkout retains all 16 chunks. No
regeneration is necessary. The 466 and /tmp/rpfbwt-64-466 references are not modified.

Measurement protocol (precommitted before production run):
- Read each chunk once, retaining ordered head/tail SA samples. O(R) preprocessing.
- Read source exactly once sequentially inside the measurement executable, applying
  the same reference remap and building resident text plus two prefix fingerprints.
  No corpus slices, external corpus scans, suffix arrays, or LF reconstruction.
- Sample 200,000 B run heads uniformly with replacement at each of 15 boundaries,
  seed 20261001. Binary search A's ordered head/tail endpoint sequence; measure
  both predecessor and lower-bound successor. Keep per-head maximum and summed
  candidate costs, plus actual binary-search comparison costs separately.
- Context = infinite local cyclic chunk text, compared to the Fine-Wilf bound
  |A|+|B|-gcd(|A|,|B|). Equality there means identical periodic streams; log separately.
  This is a local-context diagnostic, not a proof of concatenated-text order.
- Two fixed 64-bit polynomial fingerprints propose LCE via galloping and bisection.
  Each query does <=16 initial symbol comparisons, <=128 equality probes, and
  <=4096 prefix verification comparisons plus a mismatch boundary comparison.
  Total cap 10^10 charged units for the whole experiment. Exhaustion is fatal;
  no unbounded direct-verification or LF fallback. Long unverified proposals are
  counted explicitly: those observations are probabilistic, not certified exact.
- Each hash probe accesses O(1) prefix values for near-equal production chunks;
  arbitrary unequal cyclic lengths use O(log cap) hash concatenations, never walks.
- Read reference dictionary/parse once each. Measure phrase sets and frequencies
  by chunk for fully contained phrase occurrences, excluding seam phrases. These
  are projected interior dictionaries, not standalone parser outputs. Shared IDs
  denote identical original dictionary phrases, not hash-only matches.
- At the sampled nearest candidates, separately count immediately aligned equal
  phrases and equal next phrases reached inside the known common prefix. Shared
  dictionary membership alone is not treated as an LCE shortcut.
- Record exact command, seed, counters, histogram, elapsed time and maximum RSS.
  Extrapolate R=4.8e11 with explicit model assumptions, uncertainty and preprocessing.

No O(runs) merger or linear-time theorem is promised. The implementation-lane lock
will specify only the defensible comparison design and its unproved prerequisites.

## Follow-up, after the 200k pilot

Pilot completed in 104.54s / 24,141,752 KiB peak RSS. All 15 boundaries measured;
804 LCE calls were not fully directly verified because of the 4096-symbol cap.
The maximum proposal was 96,888, and boundary 5→6 showed a much heavier tail.
The pilot is retained under `pilot/` (its RESULTS.md linked DESIGN.md before the
final design was written). This evidence justifies one expanded experiment:
1,000,000 sampled B heads/boundary, seed unchanged, direct-verification cap 131,072
symbols/query, still total work <=10^10 and <=128 probes/query. Every corpus
read remains a single sequential in-tool pass. No retained artifact is modified.
The cap is fixed independently of corpus size and is <0.2% of one fragment chunk;
there is no full-text or LF walk. Longest-candidate witnesses will also be logged
as positions (no corpus excerpts) for independent checks. The final report uses
this expanded experiment, not a pooled pilot sample with duplicated observations.
