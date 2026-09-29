# Acceptance gate ledger

This is a checked experiment, **not approval for the pile rung**. The required
under-900-GB pile projection is not achieved. Independent reviewer signoff
has not occurred. No commit or staging action was performed.

- Randomized dictionary differential: PASS, 24 byte/integer dictionaries;
  exact SA, ISA, derived/materialized DA, LCP, colex and inverse-colex values.
  See `dictionary_memory_test.cpp`, `test_dictionary.py`, `dictionary-test.log`.
- Yeast235: PASS, `.rlebwt`, `.rlebwt.meta`, `.ssa`, `.ssa_t` byte-identical
  to the read-only `/tmp/rpfbwt-64-yeast/parse` reference. Time 12:08.14;
  maxRSS 5,626,544 KiB = 5.762 GB. Retained baseline: 10:02.11 and
  8,820,112 KiB = 9.032 GB. Reduction 36.2%, wall-time increase 20.9%.
- Pile 100 MB w10/p100: PASS, all four front-end files byte-identical to
  independently generated sealed-tool outputs. RSS 1.223 vs 1.747 GB;
  time 187.43 vs 150.71 s. R=38,647,349. No full-text SA/walk oracle.
- Pile 100 MB w5/p100 and w20/p100: PASS, all four files byte-identical; same R.
- Pile 100 MB w3/p5: extra low-p front-end comparison running in tool session 32344; the 100/500/1000 MB parse measurements are already complete.
- Additional window/parse experiments: consult `gates.jsonl` for completed
  gates; `RESULTS.md` can be regenerated with `summarize.py`.
- K10: RUNNING at launch, 2026-09-29 13:22 UTC; producer PID 760626,
  time-wrapper PID 760625, tool session 5598. Threads=48; RLIMIT_AS=350 GB.
  Inputs are private copies of `/mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp`.
  `run_gate.py k10` will compare `.rlebwt`, metadata and `.ssa` after exit
  and append GATE_PASS or GATE_FAIL. No retained `.ssa_t` exists in that
  historical reference. `finish_k10_tail_gate.py` is queued in tool session 64888: after the memory job passes the three retained comparisons, it runs the sealed baseline at 48 threads on another private copy and compares all four outputs. This adds one baseline front-end runtime, sequentially; the definitive event is `stage=k10-all-four,status=GATE_PASS`. Yeast and web slices already cover the tail writer.
- Disk SA probe: PASS at a hard 32 MiB cgroup ceiling with swap disabled;
  matching SA hash, 51.2x wall-time cost. This is a standalone workspace
  experiment, not the full frontend fallback. See `sa_disk_probe.cpp` and
  `sa-disk-probe.{log,time}`.
- M5 yeast gate: NOT APPLICABLE to this pin: M5 is not implemented. The
  tested memory path uses M32 where safe and M64 beyond the signed bound.
- 466 redo: NOT RUN. Conditional projection 338–355 GB, proposed budget
  380 GB; details and assumptions in `RESULTS.md`.
- Pile full-scale capacity: FAIL. At w10/p100, D projects to 1.414 TB;
  the requested hypothetical 24D is 33.93 TB. Actual changed-path SA
  packing alone projects to 21.21 TB, excluding other construction phases.

The opt-in diff should remain experimental until the K10 result and
independent review. The measured dictionary/phrase-ID growth calls for a
new full-scale construction plan; an M5 switch or a mapped temporary SA
alone cannot make this workload fit.
