# Tape reader: fixtures green; P1 not passed

**Not ready for the pile.** The native reader fixes and fixture gate are
committed, but both 3 GB P1 arms ended without `SS_DONE` and are **VOID**.
No accepted P1 ratio, P2 identity, or pile-wall projection is claimed. Work
stopped at this gate without changing knobs or attempting P2.

Source of truth: `xsa/runtime/tools/binbwt/tape_sort.cpp` on `feat/tape-reader`.
The stale `tools/binbwt` snapshot was not edited. Native gate binary source:
`4a72ee18204a32c1abd197af2bc5acffdbd76518`; binary SHA-256:
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

## P1: both arms VOID; stop

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
