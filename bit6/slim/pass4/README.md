# Pass-4 evidence

**Final status: PASS.** G0 8/8, yeast and full k10 aggregate byte identity,
expected chi counts, and NumPy sorted-set equality all passed. K10 pipeline:
5809.321 s, 73.982 GB peak; resolve 437.509 s with zero LF steps.
See [acceptance](../../../SLIM_ACCEPTANCE.md) and
[costs](../../../SLIM_COST.md).

`G0.log`, `G1.log`, `K10.log` record ordered gate commands and results.
`results.json` contains measured wall/RSS/phase summaries. Full command
stdout/stderr and GNU time are in the named phase logs.
`active.json` records the latest subprocess started by the gate runner;
its PID/PGID belongs to this lane. The runner stops only its own process
group if summed resident memory exceeds 150 decimal GB.

`k10.extract.log`: bounded-memory, one-pass full CRA1 extraction;
`k10.column-cmp.log`: full saFirst byte comparison;
`k10.lf-spots.log`: independent LF decode of 1,025 spread run heads/tails.
`extractor-test.log`: exact synthetic column and malformed-size rejection.
`invalid.*.log`: production-reader wrong-size/range rejection.
`projection.json`: corrected raw 8 B/run 466 accounting.

Large generated files remain external: /tmp/laneQ/pass4 and
/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10.{head_sa,slim.head.agg}.
The extracted head values are intentional pilot inputs from existing
aggregates; these gates do not validate a front-end sample emitter.

The first k10 attempt exposed an existing collection-LCP bug: raw PFP LCE
continued across newline terminators. Its stopped output and logs are retained
under `unclamped` names. The fix derives O(k) string-end positions during
existing dictionary/parse scans and excludes newline from collection LCP.
`k10.collection-probe.log` verifies 110,000 runs against all baseline fields;
`thread-probe/` repeats those exact LCE answers at 1/4/8/16/32/64 workers.
G0 and yeast were repeated after this fix. The final k10 uses 64 workers.

Phase walls are C++ measurements. `results.json` command walls include the
runner's polling/logging overhead; GNU time records actual process walls in
the individual logs. Pilot total wall covers dump, cmp, sweep and set check;
extraction, diagnostic probes and the stopped attempt are separate costs.
