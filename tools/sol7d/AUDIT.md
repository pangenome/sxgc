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
