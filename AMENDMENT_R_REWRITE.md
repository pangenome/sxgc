# AMENDMENT R (THE REWRITE / RUST FIRST) — spec before dispatch

Status: FROZEN. Operator order: the rewrite happens NOW, before the
C++ race-fix. The calculus: the C++ UB/race hunt is unbounded (a
layout-sensitive bug in 4,449 lines of patched scar tissue); the
rewrite is bounded (~1,200-line final shape per TAPE_VISION.md,
spec frozen, identity gates exist at every scale). The finish and
container side are ALREADY Rust (the xsa workspace) — this amendment
removes a language, it does not add one.

## R1 — Transliteration, not redesign

The Rust producer implements TAPE_VISION.md + Amendments W, B, and L
exactly as frozen. No new algorithms, no new laws, no new modes. The
C++ pfp_sort.cpp is the REFERENCE ORACLE for byte semantics during
certification and is not modified further. The level chain (the
currently-crashing part) is built FROM THE SPEC, not from the C++ —
the bug class dies with the C++ file.

## R2 — The module map

One crate in the existing xsa Rust workspace: remap, parse (the
STREAM trigger walk + phrasebook), dictsort (census buckets, dense
lex IDs), census (condensation, level count, bin mass), levels
(parse-of-parse chain, libsais via FFI at the deepest level, induce
upward), pairsbins (per-bin build/sort/emit/delete + CSR exchange +
SCRATCH_LAW accounting), emit (the concat: pre-ordered lists, the
W5 LCP carry, rayon pool), receipts (SS_* journals, DIAL_LAW,
SCRATCH_LAW, rows_sorted assertion, cooperative stop flag).
The finish/container path reuses xsa's existing Rust code.

## R3 — The kernels

libsais + libsais64 via FFI (the level-first L4 chain, by number).
Sort interiors: rayon par_sort (pdqsort-class) initially; a Rust
ips4o port is a receipt-driven follow-up, not a prerequisite.

## R4 — The gates (unchanged laws, new machine)

G0 fixtures 10/10 (the oracle stays C++ — an independent checker in
a different language is a feature) -> sandbox SHA 6325c843…f341 at
all three dial arms -> 1 GB b20ceebe… -> 3 GB nine-member identity ->
the 100 GB launch package. DIAL_LAW, SCRATCH_LAW (2xN + per
component), rows_sorted_total == phrase count, no signals, no text
to disk — all ported mechanically. Byte-identity is the law; the
product must not change, only the language.

## R5 — Port order (each step G0-gated before the next)

remap -> parse -> dictsort -> census -> levels -> LCP carry ->
pairsbins -> emit -> finish wiring. Commit per gate, push verified.

## R6 — The C++ hunt is PARKED

The ASan/bisect work stops at its current boundary; findings are
journaled (the -O0/-O2/-g triad, ASan results) in the results doc
for the record. The C++ machine remains the certified fallback at
W+B scales; the Rust machine must re-earn every anchor it claims.

## Timeline (honest, journaled)

An hour is the skeleton. Same-day to sandbox byte-identity is the
target; every module checkpoints at a gate so progress is visible.
The pile runs on whichever machine holds the full ladder when the
100 GB package is ready — certified identity, not language loyalty.
