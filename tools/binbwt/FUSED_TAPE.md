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

The packaged protocol test feeds 114 bands with K=1, including explicit
leading and trailing empty bands; every file is deleted before its consumed
acknowledgement. The resulting tiny container remains byte-identical.
`fused_scratch.py` independently samples only wave parts plus packed bands,
the exact scope of the original waves+SA+sym accounting; it starts before the
first scatter file. The main gate continues to measure generated columns,
ISA anchors and all downstream intermediates, so those costs remain visible.
All 59 packaged source hashes verify; final Cargo binaries match the gated
bundle byte-for-byte. publishBin through the tape_emit boundary is unchanged
from 41d23cf, and xsa_finish.cpp is untouched.

## 10gb measured result

SHA `22d0ce7493c6238a9d2e7f4e9a0decdf5841ff72a845d856bebaecd6d34bdaee`, chi 2676929428, runs 3474891366.
XSA_FINISH_KASAI_CLOSURE bands=53004 rank_holes=0 disorder=0 noncontiguous_runs=0 walk_fallbacks=0 max_rank_end=10000017258 n=10000017258. Product debug dump scratch is zero.
Wave/band allocated highwater: 16,099,864,576 bytes
(1.609984n), sampled once a second.
Order including column/ISA: 20,386,529,280 bytes
(2.038649n). All product intermediates:
85,521,260,544 bytes (8.552111n).
K=8, maximum observed live packed band files=8.
No full SA or standalone symbol stream was generated. Only a 4096-row
validation sample was retained; 4095 adjacent pairs pass exact sa_check.
Sample starts at rank 5000006581; global permutation is not
checked by that sample. Order/emit/finish observed walls (seconds):
2015.944/1722.923/1534.768. These are single
run operational observations; no paired N>=3 comparison, timing effect, or
speedup is established. The unchanged downstream intermediates exceed 2n,
so the measured wave/band bound is not a 2n total-product scratch claim.
Full independent STREAM_VS_BWT comparison passes with zero mismatches.
All six finish selftests and the checker selftest exit zero.

## 100 GB target-table capacity correction

The first 100 GB attempt exposed an existing emitter limit before producing
any scatter/band data or container: 22,388,157 seam targets require a 2^27-slot
table, but the hash used only 24 start-index bits. All start buckets were in
[0,2^24), forcing at least R(R+1)/2 probe steps for R=5,610,941 excess targets.
The emitter had not reached TAPE_EMIT_START. Stack attachment was unavailable;
the diagnosis follows the exact index range and target cardinality. The run
was stopped cleanly, with no container accepted.

The hash now takes the high log2(table-capacity) bits of the same Fibonacci
product, so insertion and lookup address the full actual table. Target tests,
hit accounting and every downstream guard are unchanged. The native tiny
byte gate, full BWT check and all six finish selftests pass again after this
correction. Packaged 150 MB/10 GB gates must be renewed before relaunching
100 GB; no timing improvement is claimed from this discovery.

The capacity-corrected emitter passes the renewed 150 MB byte gate with K=16:
required SHA, chi 44,034,231, runs 57,279,187, closure 0/0/0, fallbacks 0,
full STREAM_VS_BWT zero mismatches and zero product debug dump scratch.
This run uses the existing read-only reference column and sampled ISA;
the previous fully generated-column measurement remains separately recorded.

## 10gb-hashfix measured result

SHA `22d0ce7493c6238a9d2e7f4e9a0decdf5841ff72a845d856bebaecd6d34bdaee`, chi 2676929428, runs 3474891366.
XSA_FINISH_KASAI_CLOSURE bands=53004 rank_holes=0 disorder=0 noncontiguous_runs=0 walk_fallbacks=0 max_rank_end=10000017258 n=10000017258. Product debug dump scratch is zero.
Wave/band allocated highwater: 13,457,100,800 bytes
(1.345708n), sampled once a second.
Order including column/ISA: 19,968,954,368 bytes
(1.996892n). All product intermediates:
85,714,210,816 bytes (8.571406n).
K=16, maximum observed live packed band files=16.
No full SA or standalone symbol stream was generated. Only a 4096-row
validation sample was retained; 4095 adjacent pairs pass exact sa_check.
Sample starts at rank 5000006581; global permutation is not
checked by that sample. Order/emit/finish observed walls (seconds):
1688.027/1382.184/1445.371. These are single
run operational observations; no paired N>=3 comparison, timing effect, or
speedup is established. The unchanged downstream intermediates exceed 2n,
so the measured wave/band bound is not a 2n total-product scratch claim.
Full independent STREAM_VS_BWT comparison passes with zero mismatches.
All six finish selftests and the checker selftest exit zero.
