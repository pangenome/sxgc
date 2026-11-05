# Restarting tape builds

This implementation uses **verified replay from the beginning**, not incremental
recovery from a suffix-key boundary. Reissue the identical build command against
the same scratch directory after an interruption:

```sh
xsa build --tape --input corpus.bin --scratch scratch --threads 16 --memory-gb 24 --with-chi
```

A completed build verifies its container SHA-256 and returns successfully without
rebuilding. An incomplete build verifies its plan, clears build-owned working
files, and regenerates them from the unchanged corpus. Stale `.done`, `.opened`,
`.consumed`, LCP markers, and pieces can never certify work in the new attempt.
The persistent sibling scratch lock still prevents restart while any surviving
stage owns the directory. After SIGKILL, stop the remaining stages belonging to
that build before restarting; do not delete the lock inode.

`--no-resume` explicitly discards this build's previous checkpoint, intermediates,
and container and starts a new plan. Unrelated files in scratch are preserved.
A scratch directory from an older version without `checkpoint.journal` is refused
unless `--no-resume` is given. The option is supported only with `--tape`.

## Journal and publication

`scratch/checkpoint.journal` is append-only across replay attempts. Each record
is fsynced. Its initial creation and completed-container publication also sync
the scratch directory. Record types:

- `PLAN n=... corpus_sha256=... remap_sha256=... rows_per_band=... parallel_bands=... waves=... sorter_sha=... merger_sha=... finish_sha=... driver_sha=... convention=replay-v1-run-ranks knobs_sha256=... with_chi=... memory_gb=... lce_verify=...`
- `PIECE_DURABLE idx=... shard=... lo_key=... hi_key=... rows=... bytes=... sha=...`
- `REPLAY from_key=0 reason=incomplete_build`
- `BUILD_INTERRUPTED reason=... last_durable_piece=... scratch=...`
- `BUILD_DONE container=...`

The PLAN's `waves` is the resource planner's wave count; the recursive sorter may
split those waves further. `knobs_sha256` binds the inherited `CROSS_*`, `SS_*`,
and `XSA_*` environment and witness/finish/snap/chi options. Every explicitly
stored mismatched field is named in `RESUME_REFUSED field=...`. A knob mismatch
is named `knobs_sha256`. No build-owned scratch file changes on refusal.

All piece writers share one publication function: write `p-<band>-<shard>.partial`,
`fdatasync`, close, hash the exact bytes, rename, fsync the containing directory,
then append and fsync `PIECE_DURABLE`. Concurrent writers serialize journal
appends with `flock`. A failed sync, rename, hash read, or journal append fails
the stage. A crash between rename and journal append cannot cause reuse of an
unjournalled piece: replay deletes every working piece, including journalled
ones. An unterminated final record is never used as recovery state.

**The `replay-v1-run-ranks` convention matters:** `lo_key`/`hi_key` are hexadecimal
half-open run ranks, not suffix keys. `rows` sums BWT run lengths. Piece records
are publication receipts, not retained recovery objects. Normal streaming
consumers may delete these files after reading them. The last recorded piece
number is diagnostic, not an incremental recovery boundary; parallel writers
may journal pieces out of index order.

The native piece hash is dependency-free SHA-256, checked against Python hashlib
at padding and streaming-block boundaries. No build dependency was added.

## Signals

The front door catches SIGTERM (15), SIGINT (2), and SIGHUP (1). It writes a stop
request. The sorter finishes its admitted **wave** and stops before admitting
another wave. The merger drains that wave, flushes its pending piece, waits for
its piece writers, and exits. The driver journals and prints
`BUILD_INTERRUPTED reason=<number> last_durable_piece=<index> scratch=<path>` and
exits **75**, distinct from normal build errors and signal death.

The incomplete finisher is then terminated through its private process group;
its in-memory state is not a checkpoint. Restart reconstructs it from input.
If a signal arrives at the end of production, normal stage completion can drain
before the interruption is recorded. There are no wall-clock deadlines or
watchdogs. A slow admitted wave can take a long time to stop.

SIGKILL cannot be caught. Recovery does not rely on a shutdown handler or on
progress markers surviving consistently. Only an intact matching PLAN is needed
for replay. A torn initial PLAN is refused; use `--no-resume` to start anew.

## Why replay, and deviations from the requested design

The existing stream carries more state than the piece boundary exposes:
unfinished BWT runs, open seam captures and their flanks, run-rank exceptions,
LCP cursors, and the finisher's accumulated output. Saving piece files alone
would make an unsafe incremental checkpoint. This implementation chooses a
simpler invariant: immutable input plus identical tools and plan reconstructs
all state. It avoids retaining a corpus-sized SA or LCP column and keeps the
existing bounded deletion protocol.

Consequently it deliberately deviates from key-boundary resume and delayed
payload deletion: **all computation is replayed**, SA/LCP payloads keep their
existing delete-after-read lifecycle, and no pieces are retained for reuse.
Graceful shutdown drains a whole admitted wave instead of a single band and
does not preserve the current finisher fold. These tradeoffs provide a safe
restart operation, but **do not deliver incremental resume**. Repeated
interruptions of a large build repeat potentially many hours of computation.
Do not describe this implementation as the requested incremental checkpoint
scheme or infer incremental correctness from its byte-equality gates.

## Validation and retention cost

The committed integration gate runs an uninterrupted 1 GB build, kills the
front door and every descendant after piece publication, restarts, and compares
container SHA-256. It separately sends SIGTERM, checks exit 75 and the journal,
then restarts and compares again. PLAN refusal checks hashes of every scratch
file before and after a mismatched-input invocation.

```sh
cargo test --release --manifest-path xsa/Cargo.toml chain::recovery -- --nocapture
python3 xsa/tests/test_durable_piece.py
python3 xsa/tests/recovery_gate.py xsa/target/release/xsa CORPUS_1GB NEW_GATE_DIRECTORY
```

The publication tests inject fdatasync, directory-fsync, and journal-fsync
failures and independently verify the recorded hash. Measured results are recorded below and in
[the machine-readable evidence](tools/binbwt/restart_safe_results.json).

### Measured gates (2026-11-05)

Base revision: `729004c19db7dc6c9ddf7e5b0415b158bfe1c0c0`. Evidence directory:
`/mnt/nvme2n1/erikg/restart-safe-gates-20261105`. Runs used 16 threads, 24 GiB
at 150 MB / 1 GB and 48 GiB at 10 GB, with chi enabled. The 100 GB build and its
scratch directory were not modified or signalled.

- **G1 PASS:** 150,000,001 input bytes; chi **44,034,230**. `cmp` against mainline
  passed. Both container SHA-256 values are
  `f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e`.
- **G2 PASS:** 1,000,000,005 input bytes. After at least 1,500 durable piece
  records, SIGKILL killed the front door and all six descendant processes.
  Injected an unjournalled piece and a `.partial` file before restart; replay
  discarded both. Uninterrupted SHA-256:
  `f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9`.
  Resumed SHA-256:
  `f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9`.
  `cmp` also passed, including comparison with mainline's 1 GB output.
- **G3 PASS:** SIGTERM returned **75** and journalled
  `BUILD_INTERRUPTED reason=15 last_durable_piece=1665 scratch=/mnt/nvme2n1/erikg/restart-safe-gates-20261105/verified/term`.
  Uninterrupted SHA-256:
  `f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9`.
  Resumed SHA-256:
  `f5d1c4d323a73cdb9e45ea2997941fe4a5897fd8c299f13fccd92e797203c1c9`.
  `cmp` passed. Separate 150 MB SIGINT and SIGHUP runs also returned 75 and
  journalled their respective signal numbers.
- **G4 PASS:** changed corpus refused with `RESUME_REFUSED field=corpus_sha256`.
  SHA-256 of **every scratch file** was unchanged by the refusal.
- **G5:** 10 GB comparison is running; this entry is updated before publication.

Thirty chain tests and five native publication tests passed. The latter include
16 SHA-256 padding/stream-boundary cases against Python hashlib and injected
fdatasync, directory-fsync, and journal-fsync failures.

The maximum of the three completed 1 GB runs (uninterrupted, SIGKILL replay,
SIGTERM replay), compared with mainline's uninterrupted 1 GB baseline:

```text
RESUME_RETENTION peak_bytes=2356199574 vs_baseline=2278742475 overhead=1.033991
```

This is **1.033991×**, approximately **3.4% more**, using the existing 2-second
sampler's logical scratch bytes (including the container). Individual peaks:
uninterrupted 2,330,940,928; SIGKILL replay 2,356,199,574; SIGTERM replay
2,230,461,394 bytes. At 150 MB, the peak was 301,902,991 versus 301,774,821 bytes.
These are observed sampled peaks, including scheduling effects, not an exact
physical-allocation bound or an isolated estimate of journal cost.

This measures the **replay implementation**, which retains no additional SA/LCP
payload. It does not measure the requested delayed-deletion strategy, because
that strategy is not implemented. Incremental restart and 100 GB recovery
remain untested and unsupported by these gates.
