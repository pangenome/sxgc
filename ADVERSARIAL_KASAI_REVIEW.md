# Follow-up implementation and adversarial review (2026-10-26)

**Result: FAIL / incomplete.** Commit c2cfdb3 fixes unconditional stamp authority,
requires explicit `--debug-artifacts` for the two dumps and debug full inverse,
and moves the diagnostic totals after the final tail's evaluation. It does NOT
solve item 1 or establish the complete 2n space requirement. No push was made:
the requested 1 GB bank gate is not green.

I tested physically moving each seam seed first, including the streaming batch
path (a separate SeamClasses object, not just the initial class loader). This
eliminated the measured ordering breaks without fallback, but changed the
cyclic BWT and witnesses. I reverted that experiment rather than deliver a
container that passes closure while failing the requested bank. Its patch
against 0445db7 is retained in
`tools/binbwt/evidence/kasai-followup/seed-first-experiment.patch`; it is NOT the
product implementation. The installed final package contains c2cfdb3's source.

Measured seed-first experiment, Kasai ON, original closure guard enforced:

| Corpus bytes | Runs | holes/disorder/noncontiguous | reported deviating_runs | chi | emit / finish wall (s) |
| --- | --- | --- | --- | --- | --- |
| 454752 | 192138 | 0/0/0 | 0 | 145685 | 0.265 / 0.121 |
| 150000001 | 57279187 | 0/0/0 | 0 | 44034231 | 4.935 / 27.573 |
| 1073741825 | 394663025 | 0/0/0 | 0 | 303812548 | 40.664 / 218.483 |

All three reported walk_fallbacks=0. These are FAILED acceptance results:
expected chi values are 145686, 44034233, 303812551, and the 1 GB run count
must be 394663026. Experiment hashes, respectively:

* `98f1f6dc82befd1659112678bbc3debcf6c40ec417fcea86df47a8a51019c658`
* `d7eeac0f574a9a6e272732737d20b6d18a533336fea8f46f10287cb0e2b063a5`
* `46892f21cc1be91a557e6273d9f8c82a250d80a74c74582127d74e3e6cc3dad8`

The experiment inherited the old reporting location: its printed rank and
closure totals precede evaluation of the held-back tail. Its tail DID execute
through the same per-band refusal, and the final zero guard DID execute, but
the printed deviating_runs=0 is not an attestation over every final tail run.
The retained code fixes that reporting placement. I do not claim the requested
all-run audit has passed on a bank-identical container.

Independent offline fixture comparison shows why simply moving the seed is
not a byte-preserving repair: the finite-SA BWT and the bank's cyclic BWT differ
at 26 rows, beginning at rank 5615. Both have 192138 runs. The old displaced
seed at position 454751 has finite rank 5615, but the cyclic rebuild places it
at stream row 5653. Moving that actual row first rotates the intervening BWT
bytes. `test_seam_order_contract.py` reproduces the 26 differences from the
final package's explicit-debug walk dump and the existing tiny finite SA.
`order-conflict.json` records both expanded BWT hashes and the e91f9d9c...
baseline hash. This demonstrates failure of the attempted repair; it is not a
proof that every possible redesign is impossible. A design reconciling the
finite column order with the required cyclic output remains unresolved.

Final retained implementation, fresh packaged runs:

| Gate | emit wall (s) | finish wall (s) | result |
| --- | --- | --- | --- |
| A, 454752 bytes | 0.282 | 0.021 | exit 2, stamp/cursor refusal, no container |
| B, 150000001 bytes | 5.731 | 0.510 | exit 2, stamp/cursor refusal, no container |
| C, 1073741825 bytes | 37.805 | 3.673 | exit 2, stamp/cursor refusal, no container |

Closure totals, final chi, and container SHA are unavailable for these refused
runs, not zero. This is stronger refusal coverage, NOT a green acceptance gate.
Exact commands/return codes/walls are in `final-results.json`; full final emit
and refusal logs are next to it. In every run emission exited 0 first, with
`CROSS_V2_PIECE_WINDOW=100000000`, TAPE_EMIT_DONE, frag.pieces and frag.rlebwt
verified before finish started. No finish was left parked. Existing SA,
column, and sampled-ISA files were reused; they were not regenerated at scale.

Seven-check self-review against the retained code:

1. **Race: PASS (static).** `bit6/xsa_finish.cpp:4437` limits rank construction
   to worker zero. Arrival RMW at :4536 precedes release at :4586 and acquire at
   :4588; the existing pool await at :4828 precedes reuse. No race fix was
   weakened. This is not a fresh ThreadSanitizer run.
2. **Column: PASS (static; prior exhaustive fixture evidence retained below).**
   Top reads `band->at(r)` at :4601; interior reads a+1 through c at :4622.
   `LcpColumn::load` at :3784 bounds the full staged slice. The changed code
   refuses stamps BEFORE these reads. No new 1 GB independent column build
   was performed.
3. **Stamp: PASS for agree-or-refuse.** :4442-4445 check both stamps against the
   independent length cursor BEFORE the padding/straddling skip. :4459-4460
   derive h and t solely from that cursor. Stamps cannot select or override a
   rank. Both anchored and anchor-free tiny Kasai runs now refuse at the
   disagreement. Tail-stamp refusal has static coverage, not a separately
   injected runtime fault test.
4. **Closure/publication: FAIL for requested positive closure; PASS for refusal.**
   Per-band zero check at :4576 remains enforced; final guard at :4933 follows
   final tail evaluation (:4898) and precedes publication (:5318). A-C refuse
   earlier on stamp disagreement, with no product or partial output. No guard
   was disabled, hidden by an env var, or bypassed to obtain a banked SHA.
5. **Space: FAIL for the complete 2n attestation; PASS for dump isolation.**
   CLI opt-in at :3848 is required by :3918-3920 before debug inverse loading
   or dump opening. Environment variables alone refuse for all three debug
   paths, tested separately. Product-path bytes from these dumps: **0 bytes**,
   including at n=1.31e12; explicit debug remains outside that claim. No new
   stamp file or full ISA is used in final product tests. Overall product
   scratch is NOT bounded by this patch: a supplied finite SA is 8n; the
   existing column layout is 48+n+16e bytes (:3762), worst case 17n+48 =
   22,270,000,000,048 bytes at n=1.31e12, already above 2,620,000,000,000.
   I cannot give a valid total <=2n. The experiment's measured 1 GB column was
   1,304,978,753 B, PFCK 390,451,616 B, witness spill files 217,305,058 B at the
   reported checkpoint; those are not a total peak measurement or pile bound.
6. **Regression: PASS for retained walk bytes; FAIL for requested Kasai parity.**
   The final baseline/walk/explicit-debug-walk containers all match e91f9d9c...
   exactly. `test_kasai_closure.py:27` tests env-only debug refusal, then both
   Kasai variants refuse without publication. The seed-first experiment failed
   bank parity at every size and was reverted. Existing shared stamp RAM
   allocations remain on the walk path.
7. **Build honesty: PASS.** `xsa/build_tools.py:24` validates the manifest and
   :75 builds the mirrored finish. Final `python3 tools/package_xsa_sources.py`
   followed by `cd xsa && cargo build --release` succeeded (7m45s including
   waiting for the preceding build lock; 18 existing Rust warnings).
   `provenance.json` records all 59 manifest hashes matching, and byte equality
   between bit6 source, runtime mirror, and the final compiled build snapshot.
   Two earlier experimental builds failed from an accidental selftest scope
   reference; it was fixed before the successful experiment and final builds.

D: packaged repair-chunks, sweep-monoid, seam-spool exit 0 (0.286, 0.060,
0.070 seconds respectively; exact final values in final-results.json).
The regression also proves debug-dump record sizes and byte-identical walk
output with explicit debug enabled. The source/test green gate was committed
as c2cfdb3; evidence/review are committed separately.

No callable contact_supervisor/intercom tool was available: discovery by name
and description returned no matches. Thus I could not coordinate the changed
plan or request a scope decision through the required channel. No normal-text
supervisor question or approval assumption substituted for it.

The task remains incomplete: item 1 and total-space item 3, positive gates A-C,
and an all-run rank attestation on the correct 1 GB container are unverified.
The remaining historical review below describes 0445db7, not a new green claim.

---

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
