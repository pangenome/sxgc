# ENDPOINT-RULE LANE — report (/tmp/sxgc-laneE)

Task: make the sweep exact. Replace the whitelist PSV/NSV (which missed one
witness on duplicates-600k via a size-3-cell interior interposer) with exact
range-min semantics via class endpoints. All numbers gated against brute in the
same process. New files: tools/endpoint_common.py, endpoint_measure.py,
endpoint_sweep.py, endpoint_gate.py. No gated tool modified. Nothing committed.

## VERDICT: the sweep is EXACT on all 7 battery regimes — GREEN

    text               n        r     |M|    chi   built  seteq  exh-danger
    random-4-20k     20001   14959   20008   13423  13423   YES       0
    satellite-18k    18501     364   18510     322    322   YES       0
    HOR-nested       18361     232     919     214    214   YES       0
    duplicates-600k 600001   14701  164136   13004  13004   YES       0   <- the killer text, now EXACT
    dup+unique-120k 120001   53844   92806   48696  48696   YES       0
    random-bin-20k   20001    9883   19034    9653   9653   YES       0
    random-4-200k  200001  149887  199772  135968 135968   YES       0

duplicates-600k: 13,004/13,004, ZERO missing, ZERO spurious. The whitelist
counterexample's missing witness (the size-3-cell interior interposer) is now
found. Reproduce: `python3 tools/endpoint_gate.py`.

## ARCHITECTURE (no whitelist, no O(n) LCP slices, no SA, no text)

  * row space partitions into contiguous PFP class blocks (the M-classes of
    the dictionary-suffix machinery; linear rows 1..N-1 map to machinery rows
    +W-1; row 0 a sentinel block). Every block's slice minimum is evaluated
    from its TWO endpoint rows only: LCP[lo], LCP[hi-1], each computed the
    r-space way (resolve O(log) + direct-law piece law). 2|M| resolutions.
  * a segment tree over block-minima answers "rightmost/leftmost block with
    min < tau" in O(log |M|); the found block is scanned (probes counted)
    for its rightmost/leftmost sub-tau row. PSV/NSV are therefore EXACT
    (same semantics as sv()) wherever the endpoint block-min never skips a
    block that contains a sub-tau row.

## THE MEASURED LAW (and what it is NOT)

  * The raw endpoint-min statement is NOT a universal data law: interior
    dips exist in block slices (duplicates rise/dip sequences measured,
    e.g. slice [1, 563, 264, 564, 267, ...]); the per-block endpoint-min
    measurement (0 violations / 40K+ non-trivial blocks) is largely
    carried by the tiny CROSS-block value LCP[lo] (pairwise of the
    previous block's last row and this block's first).
  * The OPERATIVE law (what the sweep needs): for every threshold tau the
    sweep ever queries (tau = LCP[boundary row]) and every block inside
    the query's range, if the block contains ANY row with LCP < tau then
    at least one endpoint row also has LCP < tau.  MEASURED EXHAUSTIVELY:
    1,505,463 query-parts over the 7 texts (duplicates alone: 1,350,768)
    -- ZERO violations.  This is the warrant for exactness; the gate's
    set-equality is the end-to-end confirmation.
  * Robustness fallback if a future scale ever shows a dangerous case:
    skip iff (endpoint-min >= tau AND tau <= block mlen) — within a block
    every interior pairwise LCP >= mlen, so this can NEVER wrongly skip
    (over-enters instead; exact by construction). Costs a probe-pass over
    blocks with tau > mlen; not needed at battery scale.

## COSTS (honest; the C-term residue, precisely located)

  * BUILD: 2|M| resolutions (endpoint values). |M| is a THIRD parameter
    (dictionary-suffix classes, |M| <= sum of dictionary phrase lengths):
      duplicates-600k  |M| = 11.2r;  dup+unique 1.7r; random-4 ~1.34r;
      random-bin 1.9r; HOR 4.0r; satellite 50.9r (long-phrase dict ->
      singleton-class degeneration: |M| = n there).
  * SWEEP: probes (LCP evaluations, O(log) each) = 5.7r (random-200k),
    7.2r (dup+unique), 59.7r (duplicates-600k), 104r (satellite; = 2n —
    singleton blocks degenerate to per-row scans there — rule D's regime,
    which is exact at 1.5r with the same resolve interfaces).
  * tree ops O(log |M|) per query; witness resolutions O(r).
  * Honest total (this architecture): O(|M| + probes + r) with probes
    = O(queries x block size); block sizes avg 3.7 (duplicates), max 50.
    min(endpoint-sweep, rule D) is O(r)-constant everywhere measured.

## MECHANISM NOTES (for the Lean lane; candidate theorem)

  * Class blocks are BWT-constant in >=99% of multi-row blocks (692/78K
    exceptions on duplicates) — so blocks mostly refine runs, and LF maps
    a block's rows to a consecutive image block; the classic identity
    gives block-internal slice-min VALUE = LCP(S_lo, S_last), and the
    endpoint law is equivalent to: that min is attained at the LAST pair
    (or the cross value LCP[lo] is below everything).
  * Classic sorted-suffix identity (provable core): min of LCP[k] over
    k in (a,b] = LCP(S_a, S_b). With it, the sweep's needed law is one
    statement about sweep thresholds vs block chains: "for tau in the
    sweep's threshold set, min(block) < tau => an endpoint < tau".
    The exhaustive-0 measurement says it holds at battery scale on all
    regimes; a proof should route through the tau structure (taus are
    boundary-row LCPs = the sweep's own candidate-table values).

## NEXT

  * yeast: all inputs exist (fresh parse 34.1M occ, y2new.ri4, pieces,
    oracle chi = 85,404,240). The assembly swap is mechanical: endpoint
    build (2|M|) + gate exact-set-equality vs the chi_yeast oracle,
    cost log expected O(|M|) resolutions + probes.
  * Lean lane: state the operative law; prove via the sorted-suffix
    identity + threshold structure; then the sweep's correctness lemma.
