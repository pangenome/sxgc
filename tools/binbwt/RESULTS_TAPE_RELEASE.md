# Tape ownership and fail-fast investigation (2026-10-31)

Base: `bf7cf09` (`pile-week-v1`). Worktree: `/home/erikg/sxgc-tape-fix`.
Evidence directory: `/mnt/nvme2n1/erikg/tape-deadlock-fix`.

## Changes

- Both sorter sources accept `XSA_TAPE_WAIT_S` (takes precedence over
  `SS_BAND_WAIT_TIMEOUT_S`; default remains 3600 seconds). `SS_TIMEOUT` names
  the marker writer, file existence/size, whether the merge opened the band,
  producer phase, and the merge's private in-flight set and downstream credit.
  Atomic state-file replacement avoids partially written diagnostics.
- `tape_emit` copies one complete SA band, unlinks it and acknowledges it
  before calling `publishBin`. Multi-chunk bands no longer retain disk credit
  while downstream publication blocks. Additional memory is one packed band.
- `LcpColumn` copies each slice and its escapes, returns disk credit immediately,
  and retains private copies for the overlapping rank read. Memory telemetry
  includes those copies. Empty slices keep their immediate acknowledgement.
- The next ordered sorter claim waits for credit before arena admission;
  future worker claims do not accumulate deadline time before their
  prerequisites exist or reserve arena while waiting for downstream credit.
- `RunningPhase` owns a private process group, uses GNU timeout's `--foreground`
  mode, and kills/reaps the group on failure or early return. Previously,
  killing the `/usr/bin/time` wrapper left native consumers alive.

## Direct regressions

- `test_tape_release.py`: old merge fails after 15 seconds with a multi-chunk
  band and no finish; patched merge acknowledges immediately with piece W=1.
  Leading and trailing empty bands are also acknowledged; granting test credit
  then lets the emitter finish normally.
- `test_tape_wait.py`: missing consumer fails at XSA_TAPE_WAIT_S=.2 and prints
  the required state record.
- `test_tape_admission.py`: eight workers, K=1, .5-second deadline and a
  .1-second-per-band consumer complete 18 bands / 8,000,002 rows.
- `test_lcp_release.cpp`: slices are unlinked after a partial first fold;
  overlapping subsequent reads and a 300-valued escape remain correct.
  Build with `tools/build_slim_dump.sh` and the usual SXI dependency variables.
- `xsa/tests/test_phase_cleanup.py`: compiles the actual phase owner/spawner;
  dropping it kills the native grandchild behind time + timeout.
- `xsa_finish --selftest-stream-guard`: passes all eight refusal cases.

## Reproduction findings and limits

All large runs use fresh directories, the supplied corpora, threads=96,
K=8, a 48-GiB sorter envelope, and XSA_TAPE_WAIT_S=120. The 100-GB runs use
`--no-lcp-slices` / XSA_LCE_VERIFY=1. Each subprocess is bounded by timeout 900;
the diagnostic driver stops and terminates its groups on failure or after
band 201 is acknowledged. No full 100-GB completion was attempted.

The instrumented baseline consumed bands 66 through 72. It later failed at:

```
SS_TIMEOUT path=/mnt/nvme2n1/erikg/tape-deadlock-fix/baseline2-100gb/bands/band_112.consumed writer=emit-tape(cross_lcp_merge) band_exists=0 band_bytes=0 merge_opened=0 merge_state={pid=1315865 phase=wait-band band=112 read_rows=3004502 total_rows=3004502 inflight=none piece_next=2556 piece_consumed=2455}
```

The final 10-GB diagnostic identifies a producer-side sort, not a band retained
by the merge:

```
SS_TIMEOUT path=/mnt/nvme2n1/erikg/tape-deadlock-fix/fixed-state-10gb/bands/band_55.consumed writer=emit-tape(cross_lcp_merge) band_exists=0 band_bytes=0 merge_opened=0 producer_phase=sort merge_state={pid=1347743 phase=wait-band band=55 read_rows=5093786 total_rows=5093786 inflight=none piece_next=848 piece_consumed=830}
```

That band's scatter parts total 4,595,352,192 bytes (143,604,756 rows).
The 120-second diagnostic timeout is therefore also capable of rejecting a
long-running sort. The requested 10-GB completion/chi=2,676,929,445 and
few-second-wait gate is **not passed**. Neither this dump nor the direct
ownership regression proves the cause of the original 3600-second stall.

At inspection, native finish PIDs 951650, 3024040 and 3381707 still referenced
`/mnt/nvme2n1/erikg/wbuild/merged/frag`; two predated the latest run. The old
merge also remained alive. This establishes the wrapper-cleanup defect and
makes scratch-path interference plausible, but does not prove which process
consumed a particular old piece. Existing orphan processes were not killed;
the parent must dispose of them before reusing that scratch path. Attaching
strace was denied by ptrace permissions.

The patched 100-GB reproduction also consumed bands 0..111, then timed out
waiting for unpublished band 112 (merge `phase=wait-band`, `inflight=none`,
`piece_next=2556`, `piece_consumed=2533`). The past-band-200 gate is **not
passed**. The direct credit-release bug is fixed, but the original 100-GB
stall is not conclusively reproduced or resolved. No large-scale success or
throughput improvement is claimed.

An additional complete 8,000,001-byte deterministic corpus comparison passes
for the old merge/finish, patched merge/finish, and patched LCP-slice arm:

```
old 2beb35a0d37a6ff6453b108c4d0a99e8eff4af91cf2875643ae6af86c9539f57
fixed 2beb35a0d37a6ff6453b108c4d0a99e8eff4af91cf2875643ae6af86c9539f57
slices 2beb35a0d37a6ff6453b108c4d0a99e8eff4af91cf2875643ae6af86c9539f57
PASS old/fixed/slices products byte-identical
XSA_FINISH_DONE chi=6544643 n=8000002 r=7966303
```

The diagnostic driver and smoke comparison script are retained in the evidence
directory as `reproduce.py` and `smoke_compare.py`. Their per-run logs and
`pids.json` files identify exactly which processes were launched; all processes
launched by these drivers were terminated/reaped on exit. The old shared-run
processes listed above were outside these isolated runs.

Packaged build validation: `timeout 900 env BUILD_JOBS=16 cargo check --offline
--manifest-path xsa/Cargo.toml` passes, including rebuilding all bundled native
stages (4m 50s; existing compiler warnings). `git diff --check` passes.
