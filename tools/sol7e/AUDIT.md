# sol7e fresh-start production audit

Base 20d355e. Read the sol7b/7c/7d audits and results and the last 18 commits.
The authorization retires comparison-trace identity; fresh-start heap work is
allowed and must be measured as a signed SEAM delta against the seeded oracle.
No duplicated rows are needed by the surviving exact rank planner: OVERLAP is
zero. PLANNING is the bounded rank-search work, independently journaled.

The existing SequentialV2 sink owns mutable global runs, classes, band numbers,
and terminal validation. Merely returning S>1 would race those fields. The
implementation must retain one ordered owner for this state and bound every
producer's pending output while allowing the earliest range to drain before
completion. Local range completion must never publish global EOF.

The contact_supervisor tool is absent from the complete callable tool catalog;
no substitute messaging is used. The supplied final decision is sufficient to
proceed. No new scope decision is inferred.

The first 100 MB diagnostic was stopped during serial oracle acquisition:
scalar position reads repeatedly decode SXP4 groups. The oracle now uses
4096-row front windows, matching producer physical reads while retaining the
exact comparison operands. The 40-case fresh witness and original 40-case
seeded ordered-trace witness were rerun successfully. No scalar oracle wall
is a launch-wall measurement. The stopped gate logs are retained in scratch.

The box has sustained unrelated impg/sample_projection CPU activity. No quiet-
box performance attestation can be made from runs under those conditions.

Production dependency correction: the inherited runtime mirror did not contain
sol7a's RAM cache, borrowed spans, SIMD prefix reads, or front prefetch, although
the native source did. Preserving that divergence would leave the shipped
pipeline on the older read path. The runtime source is now synchronized exactly
with native, deliberately promoting the already banked sol7a implementation
alongside sol7e. All source-manifest entries validate. Rust driver cargo check
passed before this C++-only synchronization; native/runtime are now identical
and the synchronized runtime receives a standalone build and P=16 selftest.
