# THE TAPE READER: the pull machine

The sorter becomes four components. One sequential corpus reader, a request
queue, parallel in-memory unit sorts, and a concatenation. There are no
inter-stage protocols: the tape pauses when nothing is requested, and units
concatenate because they are already in key order.

Status: **spec**. This replaces the band/piece/window choreography of the tape
route's producer-consumer pipeline. The components below marked CARRY OVER are
already in main, proven by their gates, and are not to be rewritten.

```
component     what it is                                  status
-----------   ------------------------------------------  ------------------
THE TAPE      one sequential reader + a request queue      NEW (small)
THE PLAN      the whole radix partition, from the survey   CARRY OVER (survey) + NEW (unit builder)
THE SORTERS   parallel in-memory unit sorts               CARRY OVER (wavekey leaf sort)
THE EMITTER   unit output as pieces in the finish format  ADAPTER (small)
```

## The request protocol

A request is:

```
REQ { prefix: bytes,        // the radix node's key prefix (depth d)
      key_len: u64,        // how many key bytes to capture (first request: 8)
      max_rows: u64,       // the plan's row estimate for the node
      sink: chunk buffer }
```

The tape makes a pass when there is at least one outstanding request. During a
pass, at every corpus position `p`, it checks the outstanding requests' prefix
tables (a small per-pass automaton over the request set: match bytes at
`p, p+1, …`); a position that matches a request's prefix is routed to that
request's chunk as `(position, key[key_len])`. A position matching no request
costs one table lookup and no I/O beyond the read that was already happening.

Every outstanding request is served by the SAME pass. That is the amortisation
law of this design: **the number of corpus passes is 1 + (tie rounds), never
one per unit, never one per band.** Requests issued while a pass is running are
served on the next pass.

While extracting a row's key bytes, the extractor also records, at no extra
cost, `run_len` (the length of the run of the key's first symbol, capped,
journal cap hits) and `term` (the terminator byte after the run). These feed the
run-length digit. The extraction stops early at the terminator for run rows;
keys for ordinary rows are exactly `key_len` bytes.

## The pause

When the request queue is empty, the reader idles: it polls the queue with a
sleep, performs no I/O, sends no signals (there are none in the product). The
sorters request more data only when they have capacity, so outstanding chunk
bytes are bounded by construction:

```
outstanding_chunk_bytes <= envelope - working_reserve
```

and a plan-time refusal stands as today (a unit whose chunk cannot fit at the
maximum key length is a knob error, refused with numbers before any I/O).

## The plan

The survey (one pass, existing `subdiv_survey.h`) gives the prefix tree, run
columns, and page coverage. The unit builder partitions the whole key space
into UNITS of `--unit-target-rows` (default ~100 M rows; the arena price of a
unit is `rows × (8 + K_max + overhead)` and is printed per unit). The existing
count pass gives exact per-bin counts, which refine the survey's estimates
before the first extraction pass. The plan is a list: unit id, prefix, exact
row count, page footprint. It is journaled once, before any extraction.

Units are ordered by prefix — that order IS the output order.

## The sorters

Each sorter takes a completed chunk and sorts it entirely in memory with the
landed wavekey leaf sort: 8-bit MSD radix over the key bytes (257 buckets,
bucket 0 = virtual END), the run-length digit collapsing single-symbol classes
in one round (`(family, ±L, term, key)`), doubling tie rounds (8 → 16 → 32 → 64
bytes) that issue follow-up requests, and the LCE fallback only for the
residual. A unit whose rows all order is emitted; a unit with ties emits its
resolved prefix and waits for its follow-up chunk.

Sorters never read the corpus. Only the tape does.

## The emitter

A completed unit emits its ordered rows as pieces in the existing piece format
(same as the concat stage wrote), appended in unit order to the single output
tape. The finish, the trie tap, and the container pipeline are UNCHANGED — they
consume the ordered stream exactly as today.

## Gates

Every gate prints denominators; a run without its completion receipt is VOID.

```
P1 PASS ACCOUNTING   corpus passes == 1 + tie rounds (journal PASS n= requests=
                     rows= bytes=); rchar/corpus <= 3.0 at 3 GB with a cold cache,
                     and read_bytes/corpus == passes. This is the gate the last
                     three designs failed.
P2 ORDER IDENTITY    3 GB container byte-identical to main's binary (same flags);
                     then 100 GB byte-identical. If one band differs, STOP and
                     report the first differing unit.
P3 THE FIXTURES      the 7-fixture adversarial suite (44,040,199 rows) vs brute
                     force, unchanged from the wavekey lane, plus the repeat-run
                     fixture resolving in ONE round via the run-length digit.
P4 THE PAUSE         a run where the queue empties mid-build shows TAPE_IDLE
                     periods with zero read_bytes delta, and resumes when a
                     sorter issues its next request. Print idle_s and bytes.
P5 NO SIGNALS        strace a full build: zero signals to other pids, zero
                     execve of /usr/bin/kill. (Same gate as R1, re-run here.)
P6 NO REGRESSION     cargo tests; the finish/container/trie stages byte-identical
                     on a fixture; the governor's term table unchanged.
```

## What is deleted

The band-part and consumed-marker protocol, the piece spool between sorter and
concat, the concat stage itself, the position-window machinery, and the runtime
budget model that sized waves from `--scratch-budget-gb` × 60 % and thread
counts. The governor's term table keeps every remaining disc term (unit chunks
in flight, output tape, merged fragments, pieces, container, other) and the
plan-time bound is unchanged.

## The projection

At the pile (n = 1,307,910,802,540):

```
count pass      1.31 TB sequential                ~6 min at the measured 3.6 GB/s
extract pass    1.31 TB + ~13 G × (8+8) B writes   ~15 min
tie passes      1-2, all requests amortized        ~6-12 min each
sort            in-memory MSD over 13.08 G rows, 96 workers — CPU-bound, hours
finish          unchanged, rides along concurrently
```

The sorter stops being the bottleneck; the build takes the 100 GB rehearsal's
shape (finish-dominated) at 13× the scale.
