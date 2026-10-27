# Bounded producer/emitter tape

`scatter_sort --tape-bands DIR` replaces the full SA and symbol streams with
ordered, consumable band files. Each row is an 8-byte little-endian packed
position (low 48 bits) and carried symbol (bits 48..55). A manifest records n
and version 1. Closed files become visible through `.done`; the emitter
publishes their rows through the existing sink, unlinks the band, then creates
`.consumed`. Empty bands are acknowledged too. An end count published before
sorting the final wave covers trailing empty bands without deadlocking.

K defaults to 8. Admission of band b waits for consumption of b-K, bounding
both running and ready bands. Ascending admission is independent of worker
completion, so a slow early band cannot be starved by later bands. All bands
are drained before scattering the next wave. Each band's 32-byte scatter rows
are deleted before its 8-byte output is written; output replaces at least four
times as much disk data. Thus packed bands do not add to the wave's logical
byte highwater. A fresh tape directory is mandatory. The supervising runner
terminates the peer on failure and enforces a disk reserve before ENOSPC.

The existing optional LCP column is supported. It must be budgeted separately
from scatter rows. Runs using a generated column can lower the wave cap, e.g.
0.5n with a roughly 1.34n column on the 10 GB fixture. Allocated filesystem
blocks and markers also count in the measured result.

`fused_gate.py` runs both processes, then the unchanged finish. It samples
`du -s -B1` once per second and reports order scratch, all newly allocated
files, and intermediates excluding the container and explicit debug dump.
Existing read-only checker fixtures are reported as external inputs, never
passed to the producer or emitter. Sampling is an observed lower bound, not
an exact continuous maximum. The unchanged downstream piece/sink/finish
artifacts have their own footprint: **2n for the complete product pipeline
must not be inferred from a 2n order-stage result.**

A bounded validation sample is optional: `--order-sample-out` writes at most
1,048,576 ranks, with explicit `--order-sample-lo/count`. The runner retains
4096 central ranks (32,816 bytes including header) and runs
`sa_check --sample-first-rank LO`. This checks exact adjacent finite suffix
order, position bounds and sample uniqueness; it does not claim a global
permutation check. The full-SA checker mode keeps its original strict checks.

The first native 454,752-byte gate passes with K=1, debug/product identity,
4095 sampled adjacent pairs, full STREAM_VS_BWT comparison, closure 0/0/0,
walk fallbacks 0 and six finish selftests plus checker selftest. The container
is byte-identical to the existing fixture baseline. Tiny allocated scratch
exceeds 2n due to filesystem block rounding and its generated column; it is
not a large-corpus scratch attestation. Packaged gates and large rungs follow.
No qualified timing comparison has been made.

Packaged tiny gate also passes with K=8, all six finish selftests, checker
selftest, full BWT comparison and the identical baseline SHA. Its generated
finite predecessor LCP column is byte-identical to `/tmp/psi_tiny.col`.
The attempted 150 MB 0.5n wave cap was rejected **before scatter**: the largest
byte-pair class (space/space, 3,734,619 rows) alone requires 0.839n including the
95% wave target. The next attempt uses a 1n wave cap; the guard is retained.

The packaged 150 MB gate passes the required SHA
`d7eeac0f574a9a6e272732737d20b6d18a533336fea8f46f10287cb0e2b063a5`,
chi 44,034,231, runs 57,279,187, closure 0/0/0, fallbacks 0 and both complete
BWT/endpoint comparisons with zero mismatches. Its generated column is also
byte-identical to the existing reference column. K=8, wave cap=1n.
Observed allocated order scratch **including the generated column** is
418,557,952 bytes (2.7904n); total intermediates reach 1,344,118,784 bytes
(8.9608n). This is a negative for a 2n *total* scratch claim on this small rung.
The 10 GB measurement is required before any larger-rung scratch claim.

The driver can also request the sorter's existing sampled ISA output with
`--produce-isa --isa-stride 1024` and pass it to the unchanged finish. This is
an 8/1024-byte-per-position rank anchor, not a full inverse or a full SA.
The order sample checker rejects swapped ranks and out-of-range positions;
without the explicit sample flag it still rejects an incomplete permutation.
Its original full-SA mode passes the tiny fixture.
