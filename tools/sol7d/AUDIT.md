# sol7d state audit (base c9104a5)

The task premise differs from the implementation: the planner does NOT walk
through split points. It independently binary-searches ranks at
bit6/cross_lcp_merge.cpp:2982-3033. It owns no live heap to journal.
There is no toehold or retained LCP in this comparator/producer.

Complete logical continuation state, at the loop head after the previous
row's replacement/sift has completed:

* Immutable inputs: repaired PosOverlay arrays, corpus, sparse hash, n,
  dollar mode and hash spacing (:2858-2880, Keys :1525-1534).
* Per-side next-row cursors cur; true exhaustion ends sides[j].n
  (:3077-3079). A shard end is NOT a true exhaustion end.
* Heap active length hm, ordered values hv and ordered side IDs hs_
  (:3092-3126). The ordered array, not merely the set, is necessary.
* Global rank R and exclusive output end; ownership must stop emission at
  the range end AFTER doing that row's serial replacement/sift (:3141-3157).
  This assigns each comparison transition to exactly one output row.
* Initial heap construction comparisons belong to producer zero. A seeded
  producer must not hPush again (:3127-3128).

Other mutable state and why it is not needed for the three work counters:

* Keys::cmp locals j/cap, SideScr p/aq/r/h/pj/ready, lo/hi/step/np and scan
  offset are comparison-local (:1550-1690); each invocation resets them.
  There is no pending comparison at a loop-head checkpoint, no retained
  LCP, per-front skip count or toehold.
* thread_local cells with aq/gen/pj (:1606-1623) memoize immutable hash
  reads. They affect cell hit/miss telemetry and wall time, not comparisons,
  symbols or probes. tscr is reset before any probe (:1627-1628).
* PosColumn TL caches and clock (:1328-1372), WinBytes/WinU64 generation
  caches (:452-476, :565-610), SideText cache (:870-912), pool replacement
  epochs (:155-200) affect physical reads, not Keys' logical charges.
  Cross-thread bit-for-bit cache/timing telemetry is not claimed.
* Decoded front windows pwin/wbase (:3076-3090), prefetch step/pfAt and
  timing accumulators (:3058, :3129-3140) affect reads/speculation only.
* Budget (:1493-1508) is an additive output, not comparison input.
* Row buffering nb/out/emitted (:3058-3063) is output state, reset for each
  exclusive range; SequentialV2 run, seam, class and band state
  (:3383-3527) belongs to the ordered sink, not the comparison trajectory.
  Seeding a heap does not make this sink admit parallel producers.

No logical state is intrinsically unjournalable. Exact seeding is possible
by serial replay. The existing rank planner cannot simply journal an absent
heap. We will test replay checkpoints explicitly, charge every replay
comparison to PLANNING, and refuse to call this a bounded-work launch fix.

## Landed experiment (current source locations)

`CROSS_SEED_HEAP=1` opts into the exact-state reference construction;
S=1 bypasses it. Checkpoint type :3050-3053 stores cursors, ordered live
values and ordered side IDs (active size is values.size()). Acquisition is
a fully charged serial replay at :3056-3120. The original rank planner at
:2988-3039 still supplies boundaries only. No LCP state was added because
none exists in this walk.

The producer uses global side exhaustion at :3154, restores the journal
without hPush at :3201-3206, and stops after exactly R[s+1]-R[s] emissions
at :3212. It still performs the last owned row's replacement/sift, even
when that reads an unowned next front (:3236-3239). This is the serial
transition, not an extra halo. Producer zero owns initial heap construction,
even if its output range is empty. Repeated seams therefore do not duplicate
initialization work.

The test-only SOL7D_TRACE observer (:1531-1533, :1553-1555) logs ordered
comparison operands. The replay journal and the concatenation of producers'
logs are compared exactly at :3254-3263, with no hashes and no reordering.
Comparisons are synchronous and Keys is deterministic in its immutable
inputs, so identical ordered operands imply identical logical comparison,
symbol and probe trajectories. This does not imply equal cache telemetry
or timing.

The default native path and S=1 retain the prior charged trajectory.
The same experimental changes were applied to the Cargo runtime mirror;
its pre-existing divergence from the native source was preserved, and only
its source hash was updated. This is not a source snapshot refresh.

## Precise remaining blocker, without an impossibility claim

The requested cheap journal cannot be extracted from the existing rank
planner: it has no heap execution state. Minimal missing information is
the ordered live heap array (side identities suffice given cursors and
immutable fronts), rather than any unjournalable comparator context.
There is NO intrinsically unjournalable state found. Seeding works; the
claim that it cannot work is disproved by this branch.

The reference acquisition does a complete serial walk before launching
producers. Its exact additional charge is one serial HEAP budget, plus
rank searches. This is a valid PLANNING term, not a hidden SEAM term, but
it leaves a serial critical path and supplies no bounded-work/launch claim.
A 100% HEAP overhead is not automatically a 100% WHOLE-MERGE overhead:
prepare/repair is also part of that denominator. No 1 GB ratio is inferred.
This implementation deliberately completes replay to EOF to provide a full
reference trace; stopping replay at the last seam could save the tail but
would still leave a serial prefix. No lower bound for all possible checkpoint
acquisition algorithms is claimed.

Bounded-accounting proposal for review: retain this disabled reference as
an exact-state oracle. Require a replacement checkpoint acquisition method
to reproduce its heap journals, charge all acquisition work to PLANNING,
and gate (rank planning + acquisition + actual duplicate overlap) against
30% of the measured serial whole-merge budget per metric. Keep true side
ends and comparison-transition ownership unchanged. An alternative that
runs the serial heap once and uses its already produced rows is an ordered
serial producer, not the requested parallel walk speedup.

Production SequentialV2 still forces S=1 (:3522-3525). The experiment adds
no queues, asynchronous comparisons, or publication interleavings: journals
are immutable before existing producer threads start, and the trace vectors
are single-writer until join. The existing 472-schedule model remains the
appropriate transport abstraction, not validation of a new P-way band sink.
