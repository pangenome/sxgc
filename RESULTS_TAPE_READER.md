# Tape reader: scheduling diagnosis attested; P1 still VOID

**Not ready for the pile.** At `93f5656`, the branch-built corrected scatter
arm completes 3,000,000,001 rows, but the reader exits through its unchanged
memory guard before its first tie pass. P1 is **VOID**; its partial 2.015926
rchar/corpus ratio is not accepted. P2 and the launch projection are blocked.

The old tie-window refusal is reproduced as **OVERCOMMIT**, not a served-window
leak. Deferred plane admission passes all fixture oracles and a constrained
207-pass run with zero final live bytes. It remains a **partial scheduling
fix**: eagerly constructed request metadata is still unbounded by the ledger.
The banked 100 GB tail check is **ABSENT**, verified against the actual banked
build source and the patched predicate. Detailed third-leg evidence follows
the historical results below.

Source of truth: `xsa/runtime/tools/binbwt/tape_sort.cpp` on `feat/tape-reader`.
The stale `tools/binbwt` snapshot was not edited. The historical fixture binary
was built from `4a72ee18204a32c1abd197af2bc5acffdbd76518`, SHA-256
`725bf0fe966c1dc924838ca8876e83de930ec25cd75476ae39bf12265321dd13`.

## Design as implemented

Initial requests use the plan's radix positions. Follow-ups carry the exact
positions in each tied class; they never rederive membership by matching deep
prefixes against the corpus. Thin classes receive byte windows. Classes whose
window-plane census exceeds the per-class threshold, or which survive their
window, receive per-class dictionaries and phrase sequences: that phrase index
is the sorter's slice. The pass shares a trigger frontier. Ordering uses the
byte-walking phrase comparator, exact-period skips, widening, and bounded LCE
residuals. `prefix_relations` is a health metric, not a prefix-freeness proof;
short proper phrases still trigger a structural refusal.

For wholly periodic text with no strict-minimum triggers, a candidate period
of at most 2,048 bytes is verified against every byte during the existing
census. Phase ranks plus finite suffix lengths then order the rows exactly,
including suffixes shorter than the period. This resolves `refuse` with a
5-byte period and one extraction pass rather than widening empty phrase streams.

## Fixes

- **Docsep row loss:** with 100,000-row units, positions 6,291,431 and 6,291,432
  were lost at the end of a 6,291,456-byte corpus. The matcher reports at a
  prefix's end, but stopped scanning at the row-start bound `KEY_N`. It now
  consumes prefix lookahead while admitting only starts below `KEY_N`.
  Matcher root-child depths are initialized to one. Symbol-zero multiplicity
  was already correct: the missing rows were ordinary suffixes near the tail.
- Positional indexing reserves once per pass, removing repeated copies of all
  accumulated positions on every request.
- Removed the inherited elapsed-time/signal watchdog. The matcher refuses the
  next allocation at 1,048,576 nodes. The anonymous-memory guard exits with
  numbers; new builds observe `stop.requested` while waiting.
- Follow-up reservations are charged before their planes are allocated and
  release the exact charged amount. Dictionary bytes are charged too. A
  follow-up that cannot fit refuses with its live ledger instead of waiting
  while retaining earlier, unconsumed groups. The ledger aggregates exact
  bytes by unit and request class instead of repeatedly scanning allocation
  receipts. Under-release/over-release is a structural error.

## Fixture gate: green

24 threads; 96,000,000,000-byte envelope; 100,000-row unit target. Each run has
`SS_DONE`. Eight 6 MiB fixtures plus the small collision fixture cover
**50,348,032 corpus bytes, 50,348,041 rows, 1,175 units, zero SA mismatches**.

| Fixture | Corpus bytes | Rows | Units | SA mismatches |
|---|---:|---:|---:|---:|
| docsep | 6,291,456 | 6,291,457 | 116 | 0 |
| eofrun | 6,291,456 | 6,291,457 | 116 | 0 |
| internsep | 6,291,456 | 6,291,457 | 119 | 0 |
| r0 | 6,291,456 | 6,291,457 | 116 | 0 |
| mixed | 6,291,456 | 6,291,457 | 454 | 0 |
| period2 | 6,291,456 | 6,291,457 | 116 | 0 |
| single | 6,291,456 | 6,291,457 | 116 | 0 |
| refuse | 6,291,456 | 6,291,457 | 21 | 0 |
| trigger collision | 16,384 | 16,385 | 1 | 0 |

The collision generator is `gates/parse_gate_adversarial.c`, invoked with
`16384`; **4,112 prefix relations** were journaled. Its direct brute-force
oracle passed. The seven nonperiodic outputs match saved brute-force oracle
streams byte for byte. `refuse` matches the completed brute-force oracle
compiled with `-fopenmp -D_GLIBCXX_PARALLEL` and run with `OMP_NUM_THREADS=24`;
the byte-comparison oracle is unchanged. Its SA SHA-256 is
`02a3f603e5452063a6304308458418013f502af043e4a5c8102ce62711cf4002`.

Additional period-1/31/257 edge fixtures: **30,017 rows, zero oracle mismatches**.
`BUILD_JOBS=24 cargo test --release -j 24`: **106 passed, 0 failed, 1 ignored**.
Cargo exercises the existing product route; native gates exercise the reader.
Full fixture commands, hashes, denominators and oracle receipts are committed
in `gates/tape_reader_evidence/fixtures.json`.

## Historical P1: both arms VOID; stop

Corpus: `/mnt/nvme2n1/erikg/KEYRADIX/c3g.txt`, **3,000,000,000 bytes**, expected
**3,000,000,001 rows**. Main was built from `origin/main` (`ea90a63`) in
`/tmp/pre-tapereader`. Both arms used 24 threads, a 210,453,397,504-byte
(196 GiB) envelope, a 268,435,456,000-byte scratch budget, and the same
order-preserving remap. `posix_fadvise(DONTNEED)` ran before each arm.
Finish polling deadlines were disabled; main's inherited wait deadline was
set to infinity. No wall-clock timeout wrapped either arm.

**These are partial diagnostic counters, not completed benchmark results.**
Wall is the native pipeline wall. Neither arm has `SS_DONE`; completed-build
rows are zero for both. In particular, the reader's partial 2.015807 ratio
**does not pass P1**.

| Arm/source | rchar bytes | rchar / corpus bytes | read_bytes | syscr | Wall seconds | Status |
|---|---:|---:|---:|---:|---:|---|
| main / ea90a63 | 3,639,351,928,941 | 1213.117310 | 3,005,157,376 | 645,972,382 | 1171.870 | VOID |
| reader / 4a72ee1 | 6,047,421,983 | 2.015807 | 3,000,000,512 | 2,512 | 183.506 | VOID |

Main published 1,414 band receipts totaling **3,000,000,002 rows**, one more
than expected, then died with `scatter row coverage != n`. Concat reported
`tape bands: too many rows`. The first duplicate unit was not localized;
this was a coverage failure, not a completed P2 comparison.

The reader completed **pass 1**, serving **40 initial requests** and routing
**2,999,999,977 non-tail rows**, unmatched=0, reading **3,046,870,395 bytes**
in **152.73 seconds**. It failed while preparing follow-ups, before any tie
pass or completed unit emission. The first refusal was:

```
unit=22 class=tie-window rows=30 requested_bytes=123424
live_bytes=178485790772 cap_bytes=178485867305 remaining_bytes=76533
```

The exact live ledger at refusal:

| Request/allocation class | Units holding bytes | Live bytes |
|---|---:|---:|
| Initial chunks | 40 | 82,535,262,590 |
| Sort workspaces | 24 | 89,029,163,830 |
| Tie windows | 1 (unit 22) | 6,921,364,352 |
| Total | — | 178,485,790,772 |

The structural blocker is aggregate residency: admitted initial chunks and
sort workspaces leave insufficient space to enqueue the tie windows before
consuming them. Per-class PFP triggering does not solve that aggregate demand.
No knob was changed after this failure. Full commands, binary hashes, exits,
pass receipts and the per-unit ledger are in `gates/tape_reader_evidence/p1.json`.

Method limitation: resource flags were identical, but native output plumbing
necessarily differed (main scatter-sort + concat versus the reader's direct
pieces). This is not yet the requested integrated `xsa build` CLI comparison.
The product driver still invokes the old route. P2 at 3 GB and 100 GB is
**not run**, no identity SHA pair exists, and no pile-wall estimate is justified.

## Operational notes and remaining risks

All gate scratch is under `/mnt/nvme2n1/erikg/TAPEREADER`; no files were written
under the reserved `PILE` scratch. Long runs used `setsid nohup`. Setup failures,
the inherited-watchdog reproduction, and a cooperatively stopped oversized
collision trial are VOID, excluded from gate evidence. The collision's accepted
size is 16 KiB. Stop-file handling was observed with exit 75, without signals.

A pre-fix `c355c10` refusal run remains deadlocked: PID **734555**, RSS
**83,726,188 KiB** at the final inspection. Its stop file is
`/mnt/nvme2n1/erikg/TAPEREADER/gate-refuse/stop.requested`, but that old binary
cannot observe it inside its memory wait. No signal was sent. The new build
refuses this condition and monitors the stop file.

Redundant sequential brute-force oracle processes **740391** and **763059**
were still running at final inspection; they are not needed for the green
fixture attestation, which uses the completed 24-thread oracle and exact SA
identity. They have been left to finish without a timeout or signal.

## Follow-up diagnosis: tail predicate regression

The excess main row is localized: **offset 2,999,999,999**, raw suffix key
`20`, remapped key `21`. The original production predicate in `ea90a63`
admits it into wave 0 (depth 1, raw byte 32 in `[0,33)`) and wave 1
(depth 2, remapped byte 33 followed by virtual END). The END suffix itself,
offset 3,000,000,000, is admitted once. Several other finite tails are routed
to the wrong wave because the non-padding path compares raw bytes with a
symbol-space partition. This is a baseline predicate defect, not evidence
that the reader dropped this row.

Both census and emission predicates in the canonical
`xsa/runtime/tools/binbwt/scatter_sort.cpp` now compare remapped symbols at
all positions. No main output was filtered. The standalone regression extracts
and compiles both actual production predicates from `ea90a63` and the patched
source, then executes the recorded 20-wave plan against all 24 tails of the
3,000,000,000-byte P1 corpus. Baseline: 25 admissions; patched: 24 admissions,
each position exactly once. First **tail-only** stream divergence is rank 1:
both keys/off are `20` / 2,999,999,999 (symbol key `21`), in baseline wave 0
versus patched wave 1. This is not the first divergence of the full SA stream;
no completed reader stream exists to establish that comparison.

Reproduce: `python3 gates/tape_reader_tail_regression.py`.
Evidence: `gates/tape_reader_evidence/tail_regression.json`, including source
hashes. Native syntax compilation passes with an existing format warning at
`WAVEKEY_LCP_SAMPLE_FAIL` (`W.preLen` is unsigned but printed with PRIu64).

The second failure is still unresolved. The recorded refusal occurs before
any tie pass is served; 6,921,364,352 tie-window bytes belong to one unit.
`wavekey_sort_unit` enqueues all groups before waiting, and the tape waits for
all active sorters to park before serving any. Thus waiting inside the current
enqueue callback would deadlock, and serializing units alone does not bound
one unit's aggregate windows. Release callbacks already return billed window
and dictionary bytes after consumption. The evidence supports concurrent /
aggregate enqueue pressure, not a demonstrated post-service reservation leak.
No per-operation ledger timeline was captured by the old run; the snapshot
must not be represented as one. A bounded request-batch scheduling change and
its pass-accounting proof remain required. The cap and refusal remain intact.

**Gate status remains NOT PASSED:** no new P1 measurement was launched, no
accepted ratio, no P2 SHA pair, and no defensible pile-wall projection. The
unchanged origin/main comparator has the demonstrated tail defect; a corrected
baseline must be established before identity can attest correct order. No
claim is made about banked containers with different corpora/maps. This work
is incomplete and the pile remains blocked. No signals were sent and no files
were written beneath PILE. The stale process notes above describe the prior
run; the task setup reports those old processes are now dead.

## Banked 100 GB tail check (third-leg gate)

**ABSENT.** The frozen container's own `xsa-build.log` identifies source
`6b87aa1620505cff02bc93e6c008483d0d895a83`, predating the defective raw-byte
fast path. Its actual production predicate calls `sym_at`, which remaps every
byte and supplies virtual END. Thus the premise that this particular bank was
built with the demonstrated defective predicate is not supported by provenance.

`python3 gates/tape_reader_banked_tail.py` extracts that production predicate
and both patched production sites, compiles them, and executes the recorded
123-wave partition against the last 23 bytes plus END of the 100,000,000,000-byte
corpus. All three admit exactly 24 tails, each once, with identical wave
assignments. No duplicate and no changed assignment. The partition prefixes
are reconstructed from contiguous lexicographic interval boundaries, with an
assertion at every boundary. Evidence and source/remap/plan hashes:
`gates/tape_reader_evidence/banked_tail.json`. This bounded read-only check
supports absence of this tail defect; it is not a new full chi certificate.
The frozen container was not modified or rebuilt.

## Scheduling gate: overcommit reproduced; bounded batches validated

The instrumented 3,000,000,000-byte reproduction (base `58f17d4`, 24 threads)
reproduces the exact refusal: live **178,485,790,772**, request **123,424**,
cap **178,485,867,305**, remaining **76,533 bytes**, unit 22. Every reserve and
release is journaled with a sequence, unit, class, bytes and running total.
The replay is exact; there are **zero releases and zero served tie passes**
before refusal. This is **OVERCOMMIT**, not a served-window leak. The compressed
operation timeline and diagnosis are in `gates/tape_reader_evidence/`.

The scheduler now queues declarations without allocating window planes. At a
parked frontier it reserves demanded classes first, then prefetches the same
phase in each consumer's order while credits fit. Unadmitted classes remain
queued. Parse requests cannot displace windows that their consumer must process
first. Waits poll credits and `stop.requested` and yield; the numerical hard
refusal remains for a class that cannot make progress. The cap is unchanged.
Chunks are destroyed before returning credits. Every successful run asserts
`live == 0` and an empty per-unit ledger. An unused registry of freed request
pointers was removed, and publishing service no longer dereferences a request
after its consumer can free it.

Nine fixture streams again match the brute-force references: **50,348,032
corpus bytes, 50,348,041 rows, zero mismatches**, collision prefix relations
**4,112**. All have `SS_DONE` and zero final live bytes. A constrained test uses
**1,000,000,000 budget bytes**, 6,291,456 corpus bytes and 6,291,457 rows:
**207 passes**, 31.37 seconds native wall, exact docsep oracle SHA
`48834e29ea79d3390eafa0292756f08f92311924673759bd7051768b4e124aa7`.
Every ledger operation replays within the cap and finishes at zero. This proves
bounded progress and preserved fixture order; it does **not** prove P1's pass
or I/O requirements. Detailed receipts: `scheduling_validation.json`.

For the next P1 run, `XSA_TAPE_RCHAR_LIMIT=9000000000` is an explicit in-process
measurement guard checked after each completed reader sweep. Once this
monotonic counter exceeds 3 x 3,000,000,000 corpus bytes, acceptance is already
impossible. It exits with counters, never a signal or wall deadline. Such an
arm is **VOID**, never a completed benchmark or an accepted ratio. A bounded
16,384-byte test with limit 1 exits 2 after pass 1 with rchar 41,162 and no
`SS_DONE`. The comparison producer and reader will both be compiled from this
branch, including the tail predicate fix that must ride to main.


## Third-leg P1 verdict: corrected comparator COMPLETE, reader VOID

Both producers were compiled from this branch's source at **93f5656** with
`g++ -O3 -std=c++17 -pthread -fopenmp`. Each arm received
`posix_fadvise(DONTNEED)` before launch. Identical resource flags: 24 threads,
210,453,397,504 budget bytes (196 GiB), and 268,435,456,000 scratch-budget
bytes. Corpus: **3,000,000,000 bytes**; required rows: **3,000,000,001**.
Long runs were detached; no wall deadline or signal was used.

| Arm | rchar bytes | rchar / corpus bytes | read_bytes | syscr | Completed rows | Pipeline wall seconds | Status |
|---|---:|---:|---:|---:|---:|---:|---|
| corrected scatter / 93f5656 | 3,639,351,794,838 | 1213.117265 | 2,106,990,592 | 645,972,131 | 3,000,000,001 | 1146.128 | COMPLETE |
| reader / 93f5656 | 6,047,777,504 | 2.015926 | 2,260,570,112 | 3,171 | 0 | 301.552 | VOID |

I/O counters are producer-only; wall includes all native stages. Reader
counters are the final sampled **partial** `/proc` values, not a completed
benchmark. Scatter has `SS_DONE`, 1,414 bands, exactly 3,000,000,001 rows,
producer wall 1123.05 seconds, `XSA_FINISH_DONE`, and exits `[0,0,0]`.
The branch tail fix resolves the prior extra-row failure and must ride to main.

Reader exits are `[2,75]`: the memory guard fires, then the harness writes
`stop.requested` and the finish observes it. Anonymous RSS is **200,963 MiB**
(**210,724,978,688 bytes**) against the unchanged **200,704 MiB** guard.
The optional rchar counter guard did **not** fire. There is no `SS_DONE`.
It serves one initial sweep: 40 requests, 2,999,999,977 non-tail rows,
unmatched 0, 3,046,870,395 bytes, **152.60 seconds**. No tie pass or unit
completes. No accepted ratio or completed pass-law claim is made.

### Blocking accounting

The replayed ledger has 64 reserves, zero releases and **171,564,426,420 live
bytes**: initial chunks **82,535,262,590**, sort workspaces **89,029,163,830**.
The cap remains **178,485,867,305 bytes**. No tie plane has been admitted.
However, declarations still allocate outside this ledger before the parked
frontier: `fus`, `FollowUp::positions`, `TapeRequest`, its copied positions,
and the empty `Chunk` objects.

The 14 units that reached `TIE_GROUPS` already declared **32,179,165 groups**
covering **944,158,747 tied rows**. On this build, `sizeof(FollowUp)=152`,
`sizeof(TapeRequest)=392`, and `sizeof(Chunk)=224` bytes. Thus logical object
storage alone is **32,179,165 x 768 = 24,713,598,720 bytes**, and the two
position copies are **944,158,747 x 16 = 15,106,539,952 bytes**: at least
**39,820,138,672 bytes** before prefix storage, allocator overhead, spare
capacity or metadata in the other ten sorters. These logical allocations are
not a byte-exact RSS attribution: ledger reservations also include untouched
capacity. RSS minus ledger is **39,160,552,268 bytes** at the guard sample.
The 14 recorded units announce **3,867,274,227,712 bytes** of logical window
planes, so eager declaration and eventual sweep multiplicity both remain
material blockers. Bounding plane admission alone is insufficient.

Reproduce type sizes with `g++ -std=c++17 -O0 -pthread -fopenmp
 gates/tape_reader_metadata_sizes.cpp -o <scratch>/metadata-sizes`.
Full receipts, commands, binary hashes, per-unit counts and partial ledger:
`gates/tape_reader_evidence/p1_scheduled.json`; the complete reader log is
`p1_scheduled_reader.log.gz`. The original overcommit operation timeline is
separately retained. The next implementation must bound declaration metadata
and its concurrency before allocation, while retaining the cap and refusals.

### P2 and the pile-wall number

**P2 NOT RUN.** There is no completed reader 3 GB container to compare.
The corrected comparison container is 2,568,360,920 bytes with unpaired SHA-256
`779934fa9d0f7820aa4a3d5208155b747a66e238f441df8ceee6963edcfa26ac`.
This is not an identity pair. No first differing unit exists to report because
no pair exists. The 100 GB comparison was not launched through this failed gate.

**No defensible launch estimate.** The available extraction term scales as
`152.60 s x (13.08e9 / 3.00e9) = 665.336 s per projected sweep`.
The requested arithmetic would be
`passes x 665.336 s + 13.08e9 rows x sort_seconds_per_row + finish_seconds`.
Completed reader passes, reader sort rate, reader finish time and 100 GB P2
measurements are unavailable. The scaled sweep is from a cold-start arm, not
an isolated cold-device measurement. Filling those missing terms with zeros
or borrowing the comparator's rates would fabricate the launch decision.
**The pile remains blocked.**

Validation: `BUILD_JOBS=24 cargo test --release -j 24` passes **106 tests**, with
zero failures and one ignored. Cargo compilation overlapped early comparator
work; wall measurements are not isolated. A redundant sequential refuse
oracle (PID **830701**, parent **823768**, nice **19**) remains running because
the fixture helper missed its saved reference and that oracle has no stop-file
hook. The accepted refusal fixture uses exact identity with its committed
brute-force SHA, not this pending oracle. No signal was sent. All requested
artifact writes remain under TAPEREADER or this worktree; the frozen bank and
reserved PILE directory were untouched. This third leg is **incomplete**.
