# Adversarial review of 6233cf7

Result: FAIL against the requested zero-closure contract. No assertion is made
that a banked hash establishes container correctness. Review worktree:
`/tmp/sxgc-adversarial-6233cf7`; raw execution evidence:
`/tmp/sxgc-review-evidence/`.

1. **Race: PASS (static synchronization review).** `bit6/xsa_finish.cpp:4441`
   restricts phase 1 to worker zero. The default sequentially consistent
   arrival RMW at :4541 acquires the preceding arrivals, including worker
   zero's completed array writes. Staging publishes with release and every
   reader waits with acquire (:4591); the next dispatch follows awaitLce
   (:4835). Other workers cannot read partially populated arrays. This fixes
   the race; it does not eliminate the independently reproduced seam disorder.
2. **Column: PASS.** `tools/binbwt/scatter_sort.cpp:290,361` defines the left
   neighbor convention, including zero at rank zero. Band/wave boundaries are
   patched at :891-901. The only `Band::at` consumers in the finish are top
   `col[r]` (:4606) and inclusive interior `min(col[a+1..c])` (:4627).
   The staged slice contains both endpoints; its upper bound is checked by
   `LcpColumn::load` (:3785). The last possible read is n-1, never n. A fresh
   scatter build/output for n=454752 matched independently computed finite
   suffix Kasai values at every rank: zero mismatches, col[0]=col[n-1]=0,
   755 escapes. Evidence: `column-check.txt`.
3. **Stamp: FAIL as an unconditional rank certificate.** Rebuild binds
   runTrue entries to runs (:1701-1723), splice copies the matching index
   (:2024-2030), and coalescing retains the first head/newest tail (:1939-1946).
   A stamp deliberately overrides the cursor (:4457-4465); there is no
   stamp-versus-cursor refusal. Anchor checks refuse discrepancies only where
   the endpoint position is sampled (:4466); class verification likewise
   skips unsampled positions (:4094-4096). Sparse successful checks are evidence,
   not proof for all unsampled occurrences or all corpus families. Added a
   run/rank cardinality check before indexing to refuse malformed state.
4. **Closure/publication: FAIL in the submitted code; fail-closed fix applied.**
   Before this review the counters were incremented and printed, but never
   checked for zero. Only `lcpCol.maxEnd <= n` gated closure (submitted :4874).
   This was already true at 946aa61, and even in the original Kasai commit
   32db514 (`bit6/pile_finish.cpp:4457`). The reviewed range did NOT remove an
   existing zero-counter assertion. The requested guard was absent from the
   inspected history, despite documentation asserting ordering was checked.
   The counters were not renamed or routed elsewhere: they remained purely
   diagnostic. Walk fallback can reproduce a predecessor LCE when ranks are
   not adjacent; bank parity does not establish that the repaired stream is
   suitable for publication. Added unconditional zero checks in every Kasai
   band (:4581), and after the final tail band before finalization (:4937).
   No audit flag or anchor presence waives them.
5. **Space: PASS for incremental default on-disk scratch, not an end-to-end
   2n attestation.** New stamps add RAM: NRun grows from 40 to 56 bytes on
   x86-64; the two initially reserved 131072-run slots add 4194304 bytes.
   runTrue adds 16 bytes per rebuilt run (plus vector capacity/metadata).
   Under the existing seam total limit max(1000,r/1000), its logical payload
   is at most 16*max(1000,r/1000): at n=1.31e12 and r<=n, 20.96 GB,
   or 0.016n. Slots may grow; initial reservation is not a hard memory bound.
   No stamp file or full ISA was added to the default product path.
   Optional new dumps do break a universal scratch claim: stream=40r,
   answers=16r, together 56r; worst case 73.36 TB versus 2n=2.62 TB.
   Existing debug TrueIsa costs 8n=10.48 TB and is excluded from product use.
   The unmodified producer itself reports 8n arena-file scratch in this gate;
   this review cannot certify the whole pipeline under 2n.
6. **Regression: PASS for measured walk bytes; FAIL for literally untouched
   product code.** Without the column, the old and new walk arms produce the
   same 454752-byte corpus container, sha256
   e91f9d9c6f206e94efa96e7d1e05d980c55353cded6dc24f355b0ec9e7615aa3.
   Stamps/extra allocations still execute on that path. With a column but no
   ISA sample, rank changes intentionally still apply, and strict closure now
   refuses. Omitting anchors is not permission to publish a nonclosing stream.
7. **Build honesty: PASS for source provenance checks.** All 59 packaged
   manifest entries and every selected first-party runtime mirror matched.
   Cargo compiles runtime snapshots via `xsa/build.rs:14` and
   `xsa/build_tools.py:24-35,75`. Refreshed snapshots and manifest after fixes.

Additional reproduced defects in the author's green report:

* `--selftest-repair-chunks` exited 139. Its synthetic seam classes supplied
  runs without the newly mandatory runTrue array, and splice indexed it
  unchecked. Updated those fixtures, checked rank stamps as well as run data,
  and added the production cardinality refusal. The fixed oracle passes all
  128 streams / 3392 widths.
* Submitted `XSA_FINISH_DUMP_ANSWERS` wrote 16 bytes starting at topLcp[i].
  topLcp and interior are separate vectors: this recorded the next top value,
  and read beyond the final element. It never recorded interior[i]. Replaced
  it with an explicit two-word record. All 192138 dumped top/interior pairs
  now agree with independent cyclic LCE calculations on the fixture.

Validation:

* Fresh tiny emit completed before finish. Kasai, both with and without
  sampled ISA, exits 2 on closure and publishes no container or partial.
* Fresh 150000001-byte emit completed before finish; Kasai with sampled ISA
  exits 2 on closure. First class reports 25454 verification callbacks with no anchor
  mismatch (only sampled positions perform comparisons). Independently observed ranks jump 1775334 -> 1786466 -> 1775334;
  the seam ordering defect survives removal of the worker race.
* Added `tools/binbwt/test_kasai_closure.py` to reproduce refusal and compare
  walk bytes with a separately compiled 946aa61 baseline.

Residual risk: the underlying seam stream order has not been redesigned or
proved correct. This change deliberately refuses affected Kasai publication;
matching the historical container SHA is not a substitute for the contract.
The existing non-Kasai publication path retains its previous behavior.

1 GB gate: fresh tape emit completed in 44.70 s BEFORE starting finish, with
`CROSS_V2_PIECE_WINDOW=100000000`, 1073741825 rows, 394663027 raw runs,
183772 seam hits. Used the existing `col1g/sa` sorted SA tape and its matching
column/sampled inverse; no full ISA was loaded or constructed. Kasai was ON.
Finish refused with exit 2 at the first seam disorder, after 176212 class
verification callbacks with zero anchor mismatches (only the sampled subset
is actually compared). The observed displaced seed
rank is 12790596, emitted after predecessor end rank 12868551. No product or
partial container exists, hence no SHA match is claimed. The strict contract
precludes reproducing a green banked SHA on this stream. Raw logs are in
`/tmp/sxgc-review-evidence/one-gb-tape/{emit,finish}.log`.

An initial fresh bin-sort emitter was cancelled after approximately eight
minutes; its own 8n arena scratch was removed. The completed gate above uses
the sorted-SA tape emitter, as the tape prototype headline does. Its source
SA, column, and anchor files were reused, not independently regenerated at
1 GB; the smaller fresh scatter generation was checked exhaustively.

The orchestration instruction requested contact_supervisor first. Tool
discovery found no callable contact_supervisor/intercom tool, so no supervisor
message was sent and no approval or scope decision was assumed.

Final packaged verification (AFTER the final Cargo build):

* `cargo build --release --manifest-path xsa/Cargo.toml`: PASS, 4m29s,
  18 existing Rust warnings. Verified the built bundle's source snapshot equals
  the final bit6 source, its manifest source digest equals SOURCES.sha256.json,
  and every installed bundle file hash matches the install manifest.
* Packaged repair/sweep/spool selftests: all exit 0. Packaged tiny regression:
  PASS, including baseline byte parity and both closure-refusal variants.
* Packaged 150 MB: emit exit 0 (7.431 s); finish exit 2 (0.456 s), closure
  refusal, no publication.
* Packaged 1 GB: emit exit 0 (45.337 s process / 43.29 s reported), then finish
  exit 2 (3.017 s), closure refusal, no publication. Same 1073741825 rows,
  394663027 raw runs, 183772 seam hits; first class callback count 176212, no sampled mismatch.
* Exact command arrays, return codes, and elapsed times are recorded in
  `/tmp/sxgc-review-evidence/packaged-results.json`; logs use the corresponding
  `packaged-*.log` names. The tests used the Cargo-produced stage executables.

Resource evidence locations: NRun fields `bit6/xsa_finish.cpp:1899`, slot
reservations :4210, seam budget :1485/:1517, stamp storage :1377/:1712,
optional dump record :4228 and answer record :4703. Product-arm selection and
optional sample loading are at :3930-3944. The product walk bytes were tested
against 946aa61; no assertion is made that the whole construction algorithm
has been mathematically proved by this regression.

Further audit-count finding: `anchorChecks` in the seam rebuild is incremented
at `bit6/xsa_finish.cpp:1698` after every callback, but that callback returns
without comparison for unsampled positions at :4095. Thus SEAM_ANCHORS
`checks=176212` counts visited occurrences, not 176212 actual ISA comparisons.
The submitted report's 183772 class checks has the same overcounting issue.
The actual sampled comparison count was not separately instrumented in this
review. Endpoint anchor counts have a different implementation and increment
inside the sampled-position branch. This reinforces the rejection of an
unconditional rank certificate; the diagnostic counter itself is unchanged.
