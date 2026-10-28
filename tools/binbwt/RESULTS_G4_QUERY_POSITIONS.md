# G4: query returns correct COUNTS and correct POSITIONS (150 MB, virtual END)

The gate every other gate was blind to. chi, closure zeros, decode round-trip and
byte-identical shas all pass on a container whose coordinates are wrong; only a brute-force
comparison over the RAW corpus bytes settles it.

## Setup
- container: END-format lite container, n = 150,000,001 (+1 virtual END row), chi = 44,034,230,
  runs = 57,279,194, built by the merged tree (virtual-end-fix + reader fix).
- patterns: 100 x 24 bytes lifted from the raw corpus at seeded pseudo-random offsets, no
  re-encoding (an earlier table was poisoned by re-encoding through text, which produced three
  false "drops"). FASTA-safe (no 0x0A/0x0D/'>'/0x1E).
- oracle: every occurrence offset from an independent bytes.find scan over the corpus,
  overlapping occurrences counted (bytes.find(start = hit+1)).

## Result
    G4 patterns=100   counts_exact=100   positions_exact=100
    G4 totals: expected_occurrences=574   returned=574
    G4 VERDICT: PASS

The count histogram is deliberately hostile: 84 patterns occur once, but the table also holds
counts 2,3,4,5,6,9,10,11 and ONE PATTERN WITH 425 OCCURRENCES. A locate walk that drifts
accumulates error across hops, so a unique 24-mer can pass while a repeated pattern fails; the
425-occurrence pattern passes with every position exact.

## What this closes
Phase B. The product is queryable and the coordinates are right: string -> (count, positions)
verified against the raw corpus, not against the builder's own notion of order.

## Reproduce
    xsa query --ri4 <container> --patterns tools/binbwt/pats.fa --output occs.txt
    compare with tools/binbwt/oracle.tsv (idx, true_count, true_positions)
