# Scratch decomposition: first measurement deliverable

The unchanged-binary 10 GB repeat passes the required SHA, byte comparison,
chi 2676929428, runs 3474891366, closure 0/0/0, walk_fallbacks 0, the full
STREAM_VS_BWT comparison with zero mismatches, six finish selftests, and the
checker selftest. Both 150 MB sampler gates pass their required SHA and audits.
No C++ was edited; no package/Cargo rebuild was required. No corpus larger
than the accepted 10 GB fixture was launched. fused_gate now refuses larger
inputs before creating its run root. The existing reserve/preflight guards remain.

The repeat observes **81,658,048,512 bytes = 8.165790759n**, with
n=10,000,017,258. This is a negative for both 3n and 2n. It is a measurement
of the same accepted binaries, not an optimization or a reconstruction of the
historical 85,714,210,816-byte (8.571406289n) peak. The old aggregate trace places
that peak at 1305.497 seconds during order+emit, before finish existed. Its
component split was never recorded and cannot be recovered from deleted files.
The seven-artifact overlap hypothesis is refuted: both peaks precede finish;
sparse2, witness spills, pfck2 and container are absent at the whole-run peak.

The full time series is [10gb-baseline/scratch-components.jsonl](10gb-baseline/scratch-components.jsonl).
The [space gate](10gb-baseline/space-gate.json) reconciles its maximum to the
product gate's root maximum, emits the stack below, and exits 1 for >3n.
Directory metadata is grouped with its component. Container includes the
published file and product.sxi2.partial; named bit spools are reported separately.
Only the coincident stack adds to the root peak; independent maxima do not.

| Component | Bytes at whole peak | / n | Independent peak bytes | / n |
|---|---:|---:|---:|---:|
| waves | 4,713,021,440 | 0.471301331 | 10,351,898,624 | 1.035188076 |
| packed_bands | 159,744 | 0.000015974 | 2,349,608,960 | 0.234960491 |
| column | 9,557,270,528 | 0.955725403 | 13,758,107,648 | 1.375808390 |
| pieces | 45,866,000,384 | 4.586592123 | 47,365,914,624 | 4.736583288 |
| rlebwt | 21,443,375,104 | 2.144333810 | 21,443,375,104 | 2.144333810 |
| sparse2 | 0 | 0.000000000 | 33,423,360 | 0.003342330 |
| pfck2 | 0 | 0.000000000 | 3,636,379,648 | 0.363637337 |
| witness_spills | 0 | 0.000000000 | 2,975,350,784 | 0.297534565 |
| container | 0 | 0.000000000 | 8,587,182,080 | 0.858716726 |
| bits-head | 0 | 0.000000000 | 4,164,943,872 | 0.416493668 |
| bits-len | 0 | 0.000000000 | 941,621,248 | 0.094161962 |
| bits-high | 0 | 0.000000000 | 2,016,411,648 | 0.201640817 |
| bits-low | 0 | 0.000000000 | 403,701,760 | 0.040370106 |
| isa_sample | 78,127,104 | 0.007812697 | 78,131,200 | 0.007813107 |
| debug_dump | 0 | 0.000000000 | 0 | 0.000000000 |
| other | 94,208 | 0.000009421 | 6,094,848 | 0.000609484 |
| TOTAL | 81,658,048,512 | 8.165790759 | | |


The largest retained group is pieces. The current wire stores, per raw run,
a character byte, a 32-bit length and two packed 34-bit positions. For
3,474,891,368 raw runs, payload alone is 46,911,033,468 bytes (4.691095251n):
head-SA and tail-SA each 1.476826283n, lengths 1.389954148n, characters
0.347488537n. Rlebwt adds at least 13,899,565,472 bytes (1.389954148n).
Under the requested emit-to-completion-before-finish sequencing, these must
coexist, so **the no-column payload lower bound is 6.081049399n** before
headers, progress records, block rounding, or speculative allocation. Neither
3n nor 2n is reachable on that sequential wire. A bounded piece consumer
requires consuming during emission or changing the retained wire. Streaming
and deletion changes have not been implemented; this first deliverable does
not claim the requested redesign is complete. No supervisor/Intercom tool is
exposed to resolve that sequencing ambiguity through the requested channel.

Allocation exceeds logical payload. During emission a manual stat observed
rlebwt at 4,810,866,688 logical bytes but 8,558,215,168 allocated bytes on XFS.
Its independent allocation peak was 2.144333810n. The container partial file
peaked at 0.858716726n; the published container is 0.446378852n allocated.
These effects must be included in a future window bound.

Sampling limitation: the 10 GB run used the initial per-file traversal and a
one-second sleep. Actual order+emit median/max intervals were 1.206/7.610 s;
finish intervals were 4.764/10.683 s. This is not a strict 1 Hz attestation.
The grouped traversal and cadence correction were separately validated on
150 MB (median interval 1 s); a probe on the live 10 GB tree took 0.287 s.
No complete 10 GB rerun with the corrected cadence was performed. Du is also
non-atomic and misses open unlinked allocations, so observations are lower
bounds on continuous highwater, not an analytical upper bound. The >3n
negative remains decisive. Larger rungs remain blocked.

Evidence and residual risks: [manual-notes.json](10gb-baseline/manual-notes.json),
[results.json](10gb-baseline/results.json), [check.log](10gb-baseline/check.log).
