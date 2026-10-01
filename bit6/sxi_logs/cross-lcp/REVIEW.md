# Review evidence and limits

## Results checked locally

- Optimized C++17 compilation with `-Wall -Wextra -Wpedantic` succeeds.
- Built-in differential test: 37,970 cyclic LCE comparisons against direct
  small-string truth, with unequal lengths, wraparound, periodic equalities,
  and nearest-endpoint lower_bound checks; total-budget refusal is tested.
- Five independent Python integration tests pass: nearest-endpoint histogram
  oracle (three cyclic fixtures), seam-only dictionary exclusion, known shared
  phrase sets/frequencies/bytes, truncated chunk refusal, wrong source size refusal.
- Final measurement: 15 million uniform B-run-head draws across all 15 boundaries;
  2,531,776,149 charged units below 10^10; maximum LCE 96,914; zero incompletely
  verified queries; zero periodic-equality candidates; 174.54s wall and
  24,141,804 KiB peak RSS (about 24.72 GB / 23.02 GiB, below 200 GB).
- Summary checks all 15 boundary and dictionary records, histogram totals,
  tail-count monotonicity, candidate count/sum consistency, completion marker,
  periodic equality absence, and total budget. It produces the exact tables
  directly from the retained JSONL without corpus access.
- No edits to original frontend, SLIM_FP, retained chunks, parser outputs, 466
  reference artifacts, or unrelated workloads. No processes were killed.
- No staging or commits. `no-staged-files.log` records the final index check.

## Review findings

No blocker found in the local measurement checks. Independent reviewer acceptance
is still required; this is not a claim that an external reviewer has approved it.

Scope limits that must survive downstream summaries:
- Local cyclic endpoints are not globally reordered concatenation rows.
- The dictionary data are exact interior projections of the global parse;
  independently parsed chunk dictionaries were not created or compared.
- The dense measurement index is O(N) space; it is not the production design.
- R=4.8e11 extrapolations are conditional single-stage arithmetic, with separate
  observed search overhead, no promised full-Pile throughput or O(R) theorem.
- Zero >1M observations is not zero population probability. The zero-event
  bound is per boundary, not a simultaneous 95% bound over all boundaries.
- Shared-anchor byte hit measures overlap; no combined saving is inferred.
- The 200k pilot is retained separately and is not pooled with the expanded sample.

## Reproduction

Run from this worktree:

```
bash bit6/sxi_logs/cross-lcp/run.sh
python3 bit6/sxi_logs/cross-lcp/test_measure.py
python3 bit6/sxi_logs/cross-lcp/summarize.py
```

`run.sh` records the full measurement command in `measurement.time`. Every
measurement invocation streams the source once inside its executable. It does
not modify the source or retain extra source copies. Expected resident memory is
about 25 GB. Reproduction replaces this lane's current measurement logs; archive
those logs first if preservation of both timings is desired.

Both measurement processes have completed. There are no live long-run PIDs to
adopt; `driver.pid` is a historical record. The runtime exposed no
`contact_supervisor` capability, as recorded in PLAN.md.
