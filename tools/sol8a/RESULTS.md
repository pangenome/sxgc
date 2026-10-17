# sol8a radix sweep

Base cbcdd28; branch sol-8a-radix-sweep. contact_supervisor was searched first
in the entire callable catalog, including descriptions; it is absent. No
substitute external messages are sent. The operator's instructions authorize
this implementation. The in-flight 100GB scratch is off-limits and untouched.

## Measurement milestone (before pipeline code)

Production map: /mnt/nvme2n1/erikg/sol6c-gate100m/remap-256.bin, unchanged 0x1e.
`measure.cpp` opens the same CorpusText view as Keys::cmp. The key is a finite
suffix prefix in remapped byte order, NOT a raw corpus window, document-local
suffix, or separator-terminated prefix. Short suffixes are split from legal
zero-padded extensions. No position column exists in this measurement.
Exact 1GB results and ten largest keys/counts for every N are in
[histogram-1b.jsonl](histogram-1b.jsonl).

| N | singleton fraction | residue suffixes | residue buckets | sum b log2 b |
|---|---:|---:|---:|---:|
|3|0.000173813|999826852|637780|17713029102|
|4|0.002146196|997854468|3639145|15095139735|
|5|0.008574975|991425684|10587614|12941960716|
|6|0.023262031|976738619|20695404|11099055219|
|8|0.078026309|921974304|44537360|8059600517|

Choose N=8: the lowest projected residue sorting comparisons among the tested
widths, with the same eight-byte numeric key storage. The crucial discovery is
that **92.197% of suffixes remain in repeats**, not a small residue. The largest
bucket is 13,568,990 eight-space prefixes. Generic sorting projects ~8.06 billion
comparisons, and rechecking eight certified prefix bytes would already project
64.477 billion symbols, above the retired merge's 60,485,597,061 total symbols.
These are b log2 b heuristic projections, not measured sort counts or rigorous
comparison lower bounds; std::sort constants and long LCEs add uncertainty.
A certified-prefix skip must be journaled separately from radix work, and no
speedup follows from singleton placement alone. The 10GB measurement is deferred:
the full keys-only method requires ~170GB RAM and tenfold work; it is optional,
and the 1GB result already rejects the small-residue assumption.

D2 AMENDED BY THE OPERATOR: transient bounded rolling position segments are
allowed during radix passes; never a full SA or persistent per-suffix position
column. The design doc includes the balanced-case pile scan arithmetic and
explicitly distinguishes the 2.18TB core envelope from the >3.85TB inherited
finish overlap. The 2–3TB all-phase target is not established.

## Slice milestone

`--radix CORPUS --remap MAP --out-prefix PREFIX --position-cap 32000000`
selects the replacement producer directly; it consumes no chunks or compressed
position index. A two-symbol histogram plans contiguous key ranges. Every range
is collected in one sequential text sweep, partitioned by stable radix digits,
and emitted by numeric-key bucket order. Singleton buckets need no suffix
comparison. Residues use Keys::cmp with an explicitly certified radix prefix;
its existing borrowed-window/hash/verified-LCE path is reused. Prefix skipping
is charged as key bytes rather than re-counting them as comparator symbols.
Old callers retain certified=0 and their original comparator work.

Two RAM record banks hold at most 2B position slots (32B bytes; each slot has
an 8-byte key and an 8-byte position). B is clamped below n/2; neither a full
SA nor a persistent position file is created. Degenerate n<=2 necessarily needs
one position slot. Both banks die before the next range. An oversized initial
prefix bucket uses bounded repeated heap selection, consuming and freeing each
batch before the next scan, so periodic cyclic inputs remain correct at cap=1.
This is a fallback with poor asymptotic time, not a pile performance claim.
Emission calls the unchanged SequentialV2 row interface in strict order;
canonical pieces, flow window, band publication, seam capture and EOF remain
owned by that sink. One ordered producer is implemented; cluster scheduling
and disk-backed rolling positions are deferred.

Fresh radix brute-order oracle:
`RADIX_ORACLE_PASS cases=3270 fixtures=109 N=3,4,5,6,8 modes=cyclic,dollar cap=1,7,32 NUL=1 byte05=1 wrap_prefix_1e_0a_1e=1 periodic_giant_bucket=1 nested_repeats=1 nonidentity_remap=1 exact_brute_SA=1 bounded_selection_passes=59480`

100MB initial byte/chi gate passes (producer 69.894s, consumer complete 78.462s
on the shared box). Final source additionally fixes N>n tiny cyclic prefix
certification, and journals prefix-filter operations; the production 100MB
semantics are identical. A final-source rebuild and tiny/1GB qualification follow.
