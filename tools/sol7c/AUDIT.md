# sol7c promotion preflight (base 1c19904)

The authorized rebaseline is bounded: planning plus duplicated overlap work
may be added to the serial walk. It does not authorize replacing the serial
walk trace with a different heap trace and calling the difference overlap.

The existing candidate fails that precondition before transport changes:
`bit6/cross_lcp_merge.cpp:3079-3153` initializes a fresh heap for each range,
truncates every side at its range end, and removes exhausted sides early.
There are **no duplicated overlap rows** in this implementation. Its heap
budget can be smaller than serial for comparisons while larger for probes
or symbols. Nonnegative planning and duplicated-overlap counters cannot
account for a negative heap-work difference. Subtracting or normalizing
work to force an identity would be dishonest.

The planner itself survives at `:2975-3036`; it does exact binary rank
searches, not bounded sampled probes. Per-range fronts survive at
`:3079-3102`. Production still forces one shard at `:3422`, with mutable
global row/run/seam state in `SequentialV2` (`:3383`). The sol7b model assumes
canonical band descriptors; it does not construct those descriptors from
independent row ranges. Thus producer-agnostic transport is necessary but
does not supply parallel row-to-band production.

Historical `7a42011:bit6/cross_lcp_merge.cpp:3434-3440` explicitly describes
the sol5d overlap: TWO full heap traversals, counting and replay, overlapping
in wall time. Its `:3502-3504` creates counting and emitting sinks and
`:3561` reports `passes=2 overlap=shard`. This is not a bounded duplicated
row halo that reconstructs the serial heap state at neighboring splits.
Restoring it would violate the requested single-pass constraint.

No production source is promoted until a replacement supplies both exact
seam rows and the specified work decomposition. A new mechanism could still
exist; this audit rules out unmodified promotion, not parallelism in general.
In particular, no assertion is made that the 1 GB overhead exceeds 30%.

`work_witness.py` extends the sol7b native witness to P=1/4/8/16 and reports
signed per-metric residuals after separately charging the planner. A zero
serial metric has an undefined ratio, represented by JSON null. No aggregate
percentage mixes comparisons, symbols and probes. Native dense brute-order
and sparse seam-class checks are distinguished from production P-way gates.

Coordination: contact_supervisor was searched by name and description in the
complete callable tool catalog; it is absent. No substitute messaging was
used. The missing decision is whether a separate honest HEAP_REBASELINE
category is allowed, or whether a different exact-trace algorithm is required.
The present task explicitly permits only PLANNING and OVERLAP, so no such
permission is assumed.
