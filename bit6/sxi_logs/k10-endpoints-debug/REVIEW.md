# Review evidence (independent reviewer pending)

The failed invocation used a stale adapter and omitted the terminal argument.
The instrumented scan reports the exact firing predicate at row 0: byte 0x1e
versus the legacy newline default. An explicit 1e accepts the clean boundary.

This task changes only rpfbwt_endpoints.cpp and test_seam_repair.py, plus audit
artifacts. The adapter reads the first compressed record and its endpoint
samples, validates the known SA=n-10 row, and infers T.back() only when the
argument is omitted. Explicit mismatches refuse before outputs are opened.
Certification and emission share a padding helper. Both the clean row-10
boundary and the SA=0 dollar straddle are admitted; other straddles refuse.

The helper uses constant space/work per run; the terminal lookup is O(1).
The existing r-space seam repair and compressed-work refusal policy remain
unchanged. No corpus scan, LF fallback, tap or source front-end edit is added.
The exhaustive cyclic and dense witness oracles pass, including new omitted
terminal and malformed-boundary checks. Full separator gates pass.

No supervisor tool is available. No independent review approval is claimed.
The required reviewer gate remains pending on the supplied evidence.
