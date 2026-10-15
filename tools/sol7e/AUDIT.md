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
