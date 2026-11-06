# Restarting tape builds

This implementation **derives the plan, enumerates the spools the filesystem
already proves, and adopts them**; only what the filesystem cannot prove is
recomputed, and inside the producer/emit/finish window that still means verified
replay from the beginning (not incremental recovery from a suffix-key boundary).
Reissue the identical build command against the same scratch directory after an
interruption:

```sh
xsa build --tape --input corpus.bin --scratch scratch --threads 16 --memory-gb 24 --with-chi
```

A completed build verifies its container SHA-256 and returns successfully without
rebuilding. An incomplete build verifies its plan, enumerates the spools the
scratch holds, and regenerates the ones that are missing from the unchanged
corpus. Stale `.done`, `.opened`, `.consumed`, LCP markers, and pieces can never
certify work in the new attempt. The persistent sibling scratch lock still
prevents restart while any surviving stage owns the directory. After SIGKILL,
stop the remaining stages belonging to that build before restarting; do not
delete the lock inode.

`--no-resume` explicitly discards this build's previous checkpoint, intermediates,
and container and starts a new plan. Unrelated files in scratch are preserved.
A scratch directory from an older version without `checkpoint.journal` is refused
unless `--no-resume` is given. The option is supported only with `--tape`.

## The filesystem is the journal (driver-side resume state)

The driver models the tape route's intermediates as **spools**: a file's NAME
encodes its place in the deterministic plan (corpus + all knobs + tool hashes +
`spool_scheme`), its publication is `.part` -> fdatasync -> rename -> directory
fsync, and its **deletion is the commit record** - a spool may be unlinked only
once its consumer's output is durable, so its absence means "already
incorporated". Every attempt prints, once, exactly what the scratch says:

```text
RESUME_STATE spools_present=<n> spools_missing=<n> recomputed=<n> pieces_present=<n> resume=<true|false>
```

Spool roles:

| role | on disk | producer | consumer | absence means |
|---|---|---|---|---|
| `remap` | `remap-256.bin` (256 B, sha-pinned in `PLAN` as `remap_sha256`) | the driver, from one corpus pass | every stage | **MISSING** - it is an input, never "incorporated" |
| `band` | `bands-tape/band_<i>.sa` + `.done` | `scatter_sort` | `cross_lcp_merge --tape-bands` (`.consumed`) | incorporated, once the finish's container is durable |
| `piece` | `merged/frag.pieces/p-<band>-<shard>` | `cross_lcp_merge` | `xsa_finish` | incorporated, once the finish's container is durable |
| `container` | `<stem>.sxi2` | `xsa_finish` (fsync, then `link`) | the operator | nothing is built yet |

`recomputed=<n>` is the number of plan spools **this attempt must re-derive**.
It is never inferred from a wall clock or a skip flag.

`WORK_AVOIDED` prices the same decision in each expensive phase's own unit
(corpus bytes, corpus rows, m1 runs), and its summary carries `(T-P)/T`:

```text
WORK_AVOIDED phase=<tag> done=<n> total=<n> ratio=<r> basis=<plan|recorded|unknown>
WORK_AVOIDED_SUMMARY score=<mean of (T-P)/T> ... per_phase=remap=.. sort=.. emit=.. finish=..
```

`done` is work this attempt did NOT have to redo, `total` is what a clean build
performs, and `basis` says where `total` came from. The finish's run count is an
OUTPUT of the data, so before any committing run has recorded it the line reports
`basis=unknown(no recorded run)` and `ratio=0` - an unknown total is never guessed
and never scored as avoided work.

`SPOOL_INVENTORY` is one bounded journal record per commit carrying the counts
(`bands=`, `pieces=`) and the clean-build work totals (`work_bytes=`,
`work_rows=`, `work_runs=`). The counts come from the run's own receipts - one
`SS_BAND` line per band, one `PIECE_DURABLE` record per piece - because by commit
time every spool has been consumed and unlinked and the tree itself reports zero.

`RESUME_ADOPT_CONTAINER`: a container that exists under its final name is
complete, because `xsa_finish` `link()`s it only after `fsync`. The driver
validates it (magic, version, `n`, member-directory bounds, header CRC, and
**every member payload CRC** with the format's own polynomial) and adopts it,
journalling `BUILD_DONE`, instead of rebuilding the pipeline. A published
container that fails its own format is **corruption** and is refused -
`CONTAINER_INVALID` - never silently rebuilt.

`RESUME_INVENTORY_ABSENT` is printed when a restart finds no committing run in
the journal: the plan's band/piece COUNTS are only recorded at commit, so
`spools_missing` then counts the container plus whatever the tree shows, and the
driver says so instead of presenting a half-known inventory as complete.

### Test hook: `XSA_KILL_AT`

`XSA_KILL_AT=<point>` makes a SIGKILL-equivalent exit (no unwinding, no `Drop`,
no journal record) at a named ORDERING point, so the publication sequence is
exercised deterministically instead of by racing a signal:

- `after_rename` - every phase has exited 0, so the finish has linked the
  container into its final name, and the driver has journalled no commit. Reached
  without a poll.
- `after_fsync_before_unlink` - the container is fsynced and `BUILD_DONE` is
  journalled; no spool has been unlinked yet. Reached without a poll.
- `mid_band` - a spool exists on disk whose consumer has not incorporated it
  (pieces are the ones with a measurable lifetime; bands are consumed within
  milliseconds).

`XSA_KILL_AT` is a test hook, not a build knob: it is **excluded from
`knobs_sha256`**, so a killed run and its restart share one plan.

### One corpus pass, not five

`prepare` used to read the whole corpus five times (sha256, then two histogram
passes to derive the remap, then two more to re-derive it for the spool).
`corpus_identity` yields the sha256, the byte histogram and the length in ONE
sequential pass; the remap table and its proof are functions of that histogram;
`prepare` publishes the table as a real spool (`.part` -> fdatasync -> rename ->
directory fsync) and the chain reuses it. A resume that reuses the spool pays the
identity pass only. This is why `remap-256.bin` now survives scratch cleanup with
the container: it is the plan's own spool, not a transient.

## Journal and publication

`scratch/checkpoint.journal` is append-only across replay attempts. Each record
is fsynced. Its initial creation and completed-container publication also sync
the scratch directory. Record types:

- `PLAN n=... corpus_sha256=... remap_sha256=... rows_per_band=... parallel_bands=... waves=... sorter_sha=... merger_sha=... finish_sha=... driver_sha=... convention=replay-v1-run-ranks spool_scheme=1 knobs_sha256=... with_chi=... memory_gb=... lce_verify=...`
- `PIECE_DURABLE idx=... shard=... lo_key=... hi_key=... rows=... bytes=... sha=...`
- `SPOOL_INVENTORY scheme=1 remap_sha256=... bands=... pieces=... work_bytes=... work_rows=... work_runs=...`
- `REPLAY from_key=0 reason=incomplete_build`
- `BUILD_INTERRUPTED reason=... last_durable_piece=... scratch=...`
- `RESUME_ADOPT_CONTAINER path=... bytes=... sha256=... validation=...` (a published, unjournalled container accepted by format validation; followed by `BUILD_DONE`)
- `BUILD_DONE container=...`

The PLAN's `waves` is the resource planner's wave count; the recursive sorter may
split those waves further. `knobs_sha256` binds the inherited `CROSS_*`, `SS_*`,
and `XSA_*` environment and witness/finish/snap/chi options. Every explicitly
stored mismatched field is named in `RESUME_REFUSED field=...`. A knob mismatch
is named `knobs_sha256`. `XSA_KILL_AT` is the one `XSA_*` name excluded from it
(it is a test hook, not a build knob). No build-owned scratch file changes on
refusal.

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
payload deletion: **the pipeline's computation is still replayed from zero after
an interruption inside the producer/emit/finish window**, SA/LCP payloads keep
their existing delete-after-read lifecycle, and no pieces are retained for reuse.
The driver-side state machine above makes that boundary explicit and measurable
(`RESUME_STATE`, `WORK_AVOIDED`) instead of implicit, and it removes the replay
entirely in the two cases where the filesystem already proves the answer: a
committed container (b, d) and a published-but-uncommitted one (e).

**The native changes that would remove the (f) replay.** Not made here - the
native stages are owned by another lane - but named exactly:

1. `scatter_sort --tape-bands` refuses a pre-existing `bands-tape/manifest`
   (`O_EXCL`) and any pre-existing `band_<i>.producer-state` marker, so published
   band spools cannot be handed back to it. It needs a resume argument: "bands
   `[frontier, N)` are already published and contiguous; verify their `.done`
   markers and headers, keep them, start at the frontier". Band identity is
   deterministic (band `i` is rows `[i*rows_per_band, ...)` of the same corpus and
   the same plan), so that is a validation, not a re-derivation.
2. `cross_lcp_merge --tape-bands` unlinks each band at read time. It needs to hold
   a band until its pieces are durable, and to skip pieces already present in
   `merged/frag.pieces/` (their headers carry band, shard, `firstRun` and run
   count, so presence is verifiable).
3. `xsa_finish` unlinks each piece as it folds it. It needs the delayed-deletion
   protocol from the resume design - delete piece `p` only after the container
   prefix covering it is durable - plus the per-phase op counters that
   `WORK_AVOIDED` currently has to read out of the phase logs.

With those three, a restart would count the surviving spools as present, price
`WORK_AVOIDED` per phase from the native counters, and re-derive only the missing
spools - which is exactly the state `RESUME_STATE` already reports.

Graceful shutdown drains a whole admitted wave instead of a single band and
does not preserve the current finisher fold. These tradeoffs provide a safe
restart operation, but **do not deliver incremental resume**. Repeated
interruptions of a large build repeat potentially many hours of computation.
Do not describe this implementation as the requested incremental checkpoint
scheme or infer incremental correctness from its byte-equality gates.

### Measured gates (2026-11-06): driver-side resume state

Base revision: `803bfd2` (pile-week-v1 `finish: checkpoint entropy spools and chi
bases at quiescent bands` + this lane). Corpus: 20,000,000 bytes, the head of
`/dev/shm/t150m.txt`. 16 threads, 24 GiB, chi enabled. The box was running other
lanes' work throughout (load average 52-64), so every wall carries the box load
it measured under: **counters are exact, walls are contended**. Binary under
test: `xsa` sha256
`8d95ca6ab2945b482bf8f500161bf791206815963d44fad7600ac363087d1454`
(`cargo build --release`, 29 pre-existing warnings, 0 errors).

- **(a) clean build:** **12.26 s** (load 52.82).
  `RESUME_STATE spools_present=0 spools_missing=0 recomputed=0 pieces_present=0 resume=false`
  Container `de8cc0313563566ec8c5d44d65c9c1de589773a80708633043216a0ef5a4bfa0`.
- **(b) restart, same scratch, no `--no-resume`:** **0.18 s** (load 59.32).
  `RESUME_STATE spools_present=2 spools_missing=0 recomputed=0 pieces_present=0 resume=true`
  `WORK_AVOIDED phase=remap done=20000000 total=20000000 ratio=1.000000 basis=plan=corpus_bytes`
  `WORK_AVOIDED phase=sort  done=20000001 total=20000001 ratio=1.000000 basis=plan=corpus_rows+END`
  `WORK_AVOIDED phase=emit  done=20000001 total=20000001 ratio=1.000000 basis=plan=corpus_rows+END`
  `WORK_AVOIDED phase=finish done=7930753 total=7930753 ratio=1.000000 basis=recorded=m1_runs`
  `WORK_AVOIDED_SUMMARY score=1.000000 ... per_phase=remap=1.000000 sort=1.000000 emit=1.000000 finish=1.000000`
  Same container SHA-256.
- **(c) delete ONE spool (`remap-256.bin`), restart:** **0.16 s** (load 59.32).
  `RESUME_STATE spools_present=1 spools_missing=1 recomputed=1 pieces_present=0 resume=true`
  `WORK_AVOIDED phase=remap done=0 total=20000000 ratio=0.000000` (the one phase
  that recomputed), the other three `ratio=1.000000`,
  `WORK_AVOIDED_SUMMARY score=0.750000`,
  `RESUME_SPOOL_REPUBLISHED path=.../remap-256.bin sha256=f73af42fb5b7a1a946c9199919cdbcafaca31f6e904a56d31b530ec662580f11 role=input recomputed=1`
  - the republished spool is bit-identical to the deleted one, exactly one spool
  was recomputed, and the container SHA-256 is unchanged
  (`de8cc031...` before and after).
- **(d) `XSA_KILL_AT=after_fsync_before_unlink`, then restart:** the killed run is
  12.18 s (load 59.32). State left by the kill: the journal ends
  `SPOOL_INVENTORY scheme=1 remap_sha256=f73af42f... bands=31 pieces=122 work_bytes=20000000 work_rows=20000001 work_runs=7930753`
  then `BUILD_DONE container=de8cc031...`, and `bands-tape/`, `waves-tape/`,
  `merged/`, `remap-256.bin` and the container are all still on disk. Restart
  **0.16 s** (load 62.52):
  `RESUME_STATE spools_present=2 spools_missing=0 recomputed=0 pieces_present=0 resume=true`
  `WORK_AVOIDED_SUMMARY score=1.000000` - state self-consistent, no silent loss,
  same container SHA-256.
- **(e) `XSA_KILL_AT=after_rename` (published, uncommitted), then restart:** the
  killed run is 9.37 s. Restart **0.19 s** (load 62.52):
  `RESUME_ADOPT_CONTAINER path=.../c20m.sxi2 bytes=16585320 sha256=de8cc031... validation=sxi2-header-crc+member-bounds+length`,
  `RESUME_STATE ... resume=true`, `WORK_AVOIDED_SUMMARY score=1.000000`.
- **(f) `XSA_KILL_AT=mid_band`, then restart:** killed at 2.57 s with **8 piece
  spools** on disk. Restart **12.19 s** (load 63.74):
  `RESUME_STATE spools_present=9 spools_missing=1 recomputed=1 pieces_present=8 resume=true`
  `RESUME_INVENTORY_ABSENT reason=no_committed_SPOOL_INVENTORY required_known=0 ...`
  `WORK_AVOIDED_SUMMARY score=0.250000` (only `remap` avoided; sort/emit/finish
  fully replayed). Same container SHA-256. **This is the honest limit** - the
  native changes that would remove it are listed below.
- **(g) `--no-resume`:** **12.24 s** (load 52.13), `resume=false`, full clean
  rebuild, same container SHA-256.
- **(h) corrupted published container:** one payload byte flipped at offset
  8,000,000 of an adoptable container (state built by the `after_rename` kill).
  The restart REFUSES and exits 1:
  `xsa: error: build: CONTAINER_INVALID .../c20m.sxi2 member 1 payload CRC f9ab7f6a != stored 7a55b3fb (a published container that fails its own format is CORRUPTION: refusing rather than silently rebuilding)`
- **150 MB byte-identity gate:** `/dev/shm/t150m.txt`, clean **70.52 s** (load
  72.58), chi **44,034,230**, container
  `f6abd41da6e3c3797abae2e288596a6042337a894869f573fecf51c53f624b3e`, `cmp`
  identical to mainline's `base150` container byte for byte. Same-scratch
  restart **0.53 s**:
  `RESUME_STATE spools_present=2 spools_missing=0 recomputed=0 pieces_present=0 resume=true`
  `WORK_AVOIDED phase=finish done=57279194 total=57279194 ratio=1.000000 basis=recorded=m1_runs`,
  `WORK_AVOIDED_SUMMARY score=1.000000`.

The 20 MB numbers above come from an uninterrupted sequence
`a -> b -> c -> a' -> d -> d' -> e -> e' -> f -> f' -> h -> h' -> g` in one script
(`run_gate.sh`), one binary, one corpus.

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
