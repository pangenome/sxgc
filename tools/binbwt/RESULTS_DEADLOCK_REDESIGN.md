# Tape redesign qualification (2026-10-31)

Base: `82a1e449a87646c98482853da98b57695e20216a` (`pile-week-v1`).
Worktree: `/home/erikg/sxgc-tape`, branch `tape-deadlock-free`.
Evidence: `/mnt/nvme2n1/erikg/tape-redesign`.

**This is a partial implementation, not an attestation of full deadlock freedom.**
The LCP side-channel proof remains open. In-band subdivision does not create
independently scheduled smaller band jobs, and large waits still occur.
`DEADLOCK_FREEDOM.md` states the target invariants, the reserved-window argument,
the actual implementation boundary, and the required remaining wire changes.

## Implemented and checked

- Manifest-v2 bounded completion reception, key-ordered final emission, and
  admission from any returned band credit. The legacy slice route keeps v1.
- Aggregate reorder byte bound from the fixed staging allowance; actual row
  counts shorten windows. The merge checks the bound before allocation.
- Finish returns piece credit immediately after copying and validating its raw
  runs, before repair/LCP/verification work. Existing LCP read-time release is
  retained; its independent producer credit remains a proof blocker.
- Nonterminating SA-credit diagnostics, default 1800 seconds with the requested
  override. Legacy LCP credit growth/timeouts are explicitly not certified.
- Next-byte refinement of large equal-key groups, including an exact uniform-run
  shortcut. Both standalone and packaged sorter sources contain the change.
- Every completed wait is timed. Packaged sort timing now includes the actual
  sort rather than only post-sort work. The runner records commands, exits,
  hashes, splits and the five slowest completed bands and owns process cleanup.

## P1: passed

Current reviewed binaries, unchanged pre-change baseline artifacts, exact `cmp`:

| Corpus | chi | SHA-256 of identical product |
| --- | ---: | --- |
| deterministic 8 MB | 6,544,643 | `2beb35a0d37a6ff6453b108c4d0a99e8eff4af91cf2875643ae6af86c9539f57` |
| `/dev/shm/t150m.txt` | 44,034,230 | `9ba2b1a11b02397e8b8a3992bb3366e933a35d1929b0849898178ecc7f31a756` |

The reviewed runs are `reviewed-8mb` and `reviewed-150mb`; the baseline 150 MB
run is `p1-150mb-baseline`. The 8 MB baseline is the retained pre-change
`tape-deadlock-fix/smoke2/fixed/product.sxi2` artifact. Each run records its
complete commands. Maximum completed waits were 0.024 s and 0.040 s respectively.
An initial 150 MB attempt used a remap for the wrong corpus and was correctly
refused; it is not counted as a successful gate.

## Direct regressions: passed

- `test_tape_any_credit.py`: band 1's acknowledgement admits band 2 while band 0
  remains unread.
- `test_tape_reorder.py`: band 1 is read and released before band 0 even exists;
  ordered emission and a short final window complete.
- `test_tape_release.py`: multi-chunk band released before downstream credit,
  including leading/trailing empty bands.
- `test_tape_split.py`: exact finite suffix order and BWT symbols for mixed
  uniform runs, uniform text, periodic text, repeated prefixes and random text;
  both packaged and standalone sorter binaries pass.
- `test_tape_wait.py`: the diagnostic fires and the producer stays alive.
- `test_tape_admission.py`: K=1 with a delayed consumer completes all rows.
- `test_tape_progress_model.py`: all reachable abstract schedules for seven
  bands, K=1..4 and P=1..3 have a runnable transition or are complete (12 cases).
  This does not certify the legacy LCP or seam protocols.
- `xsa_finish --selftest-stream-guard`: all eight refusals pass.
- Final `cargo check --offline --manifest-path xsa/Cargo.toml`, BUILD_JOBS=16:
  passes in 5m18s, with the existing 27 Rust warnings.
- `git diff --check`: passes.

## Large-run evidence

The final large runs are `bounded-10gb` and `bounded-100gb`, both with K=8,
96 threads, a 48-GiB sorter envelope, piece window 1100, and
`XSA_TAPE_WAIT_S=180`. The 100 GB corpus is opened read-only and is unchanged.
The large binaries precede only the final diagnostic-state additions and strict
manifest row-capacity parsing; their scheduling, sorting and memory bound match
those in the reviewed small gates. Binary digests are recorded with the summary.

The earlier nominal-quantile row bound refused the 10 GB band's actual
143,604,756 rows. That failed attempt is `final-10gb`; it is not a successful
memory or product gate. The subsequent fixed staging allowance is declared at
startup, not enlarged after a wait.

At the 100 GB long wait, band 112 was still sorting while bands 113..119 were
already read and released, and 120..126 were published. A one-second `/proc`
thread CPU sample measured 100 scheduler ticks on the sorting worker. Band 112
subsequently completed its sort in 322.069976 seconds without a watchdog exit.
That observation is consistent with expensive finite local sorting; it is not
proof of the complete wait-for graph or a successful latency gate. Attempted
`gdb` attachment was unavailable (`ptrace: Inappropriate ioctl for device`).

Final large-run status and distribution are recorded in
`evidence/deadlock-redesign/summary.json` and in the qualification sections below.

## P2: product passed, latency failed

The 10 GB run completed with chi **2,676,929,445**, n=10,000,017,259,
r=3,474,889,847, and all three stage exit codes 0. The container is
6,178,186,696 bytes; SHA-256 `97dbf45dc6ad42aaf124a566061a818d0a7f1bc72cc91368b883546d71f899bf`.
Elapsed pipeline time was 1679.386 s. The maximum completed credit
wait was **116.028560 s**, so the required seconds-only
latency gate is **not passed** despite the correct final product.

The packaged sorter reported peak owned RSS / envelope = 0.5240 and planned
arena ratio = 0.8063. This verifies its existing budget journal for this run,
not a combined memory bound for all three processes.

The separate owner observed all native stages exit normally, then resumed the
Python monitor to reap them. Its original experimental deadline had elapsed,
so the unmodified runner printed `deadline` with `[0, 0, 0]`; the independent
summary uses the exit codes and final checked product, without claiming that
the latency criterion passed. All processes started for qualification have
been terminated or completed and reaped.

## P3: failed

All bands 0..352 were consumed (353 bands). Band 401 was not reached.
The maximum completed credit wait was **294.621662 s**; the seconds-only latency
requirement is not met. Sort-time distribution across the 353 completed bands:
p50 2.098953 s, p95 11.367675 s, maximum 322.069976 s.

| Band | Rows | Sort seconds |
| ---: | ---: | ---: |
| 112 | 3,489,180 | 322.069976 |
| 150 | 50,025,789 | 35.580794 |
| 254 | 11,974,892 | 27.838272 |
| 111 | 3,004,502 | 23.547671 |
| 69 | 7,681,099 | 22.300200 |

351 bands recorded in-band subdivision. This does not satisfy the requirement
for independently scheduled smaller band jobs. In the second wave, band 356
contained 339,607,712 rows (2,716,861,696 packed bytes), exceeding the fixed
268,435,456-row / 2-GiB reorder allowance. The sorter refused it with exit 2;
the pipeline owner terminated and reaped merge and finish (both -15).
The supervisor detected that failure before the planned manual stop.

Both large experiments were allowed more than the runner's original 1200-second
experimental deadline, within the task budget. A separate supervisor paused
only the Python monitor and kept observing the native processes, then resumed
it for cleanup. No native stage was paused, no queue was enlarged after a wait,
and XSA_TAPE_WAIT_S stayed 180. The raw runner therefore labels its eventual
cleanup `deadline`; `operator-supervision.json` and the stage return codes show
the actual failure/completion reason. These records are summarized without
turning the failed latency gate into a pass.

## Merge decision

Do not merge this branch as the requested certified redesign. P3 has a concrete
blocking failure on the supplied corpus, and the legacy LCP side-channel proof
is incomplete. The branch contains the implementation, design, P1 evidence,
large-run measurements and instrumentation for further work. The required
remaining work is actual band-job subdivision before admission, LCP transport
through merge-owned piece envelopes, and renewed P2/P3 qualification.
