# Radix sweep: operator-directed replacement, sol8a

Base cbcdd28. The comparison merge is retired for this new producer. The
existing publisher, flow window and fused finish remain the output contract.

## Law and amended D2

The corpus is the only text. Every text read uses CorpusText windows, including
its inherited immutable RAM cache. No text, radix keys, or sorted text is written
to disk. D2 AMENDED BY THE OPERATOR: suffix positions may materialize transiently
in bounded rolling segments during radix passes; never a full SA, never a
persistent per-suffix position column. The implementation must enforce a numeric
position cap, release each segment before the next and journal its high water.
Keys-only histogram arrays contain no positions and are RAM-only measurement
instruments, not proposed pile-scale structures.

## Pile arithmetic (decimal units)

Let n=1,310,000,000,000 bytes, scan throughput=2,000,000,000 bytes/s.
One corpus sweep costs 655s=10.92min before CPU and read contention.
For a rolling position allowance B=32,000,000,000 positions, eight-byte positions
occupy at most 256GB. There are at least ceil(n/B)=41 product segments. Each
segment is completely sorted and emitted in key order before the next is
materialized. A count/planning sweep plus 41 collecting sweeps reads 42n=55.02TB,
27,510s=7.64h at the stated isolated sequential bandwidth. Skew, recursive
partition counting or bounded selection of an oversized equal-key bucket adds
passes; 41 is a balanced-case bound from below, NOT a worst-case promise.
Partition by key ranges, never text ranges: text range partitions can independently
count/collect into the same active key range; final ordering is concatenation of
key ranges. No heap of sorted producers is necessary.

A future disk-backed position-only rolling segment writes and rereads 8n=10.48TB
each over the full build, regardless of the segment cap. At 2GB/s this adds
10,480s=2.91h; the nominal scan plus position transfer service time is 10.55h,
before residue work, finish and product I/O. Two sort banks require 512GB live,
not 256GB; in-place sorting or RAM sorting is required to use the smaller figure.
The vertical slice uses RAM records and does not implement this disk pass scheme.

From the inherited 1GB bank, container ratio=467814822/1000000665=0.467814511n:
pile product ~612.837GB. Text + one 256GB position window + product =2.178837TB.
Two position banks give 2.434837TB. This is the requested 2–3TB core envelope,
conditional on product ratio and bounded additional state. It is NOT yet an
all-phase disk attestation with the unchanged finish.

The unchanged finish retains its rlebwt for container replay (four bytes/run;
1GB r=368036609 =>1.472145457n =>1.928511TB at pile). At container completion,
source + rlebwt + container alone total ~3.851348TB, exceeding 3TB even after
all position segments are gone. The inherited pfck allowance .364n adds
476.84GB while live; witness spill and W-limited pieces are additional.
A default k=32 sparse hash over the virtual doubled corpus adds .5n=655GB
while the residue comparator is live. Neither the operator's radix keys nor
windowed cache removes these inherited costs automatically. The core target is
plausible; an unconditional 2–3TB all-phase claim with UNCHANGED finish is false
at the measured run ratio. Future work needs an explicit decision about compact
rlebwt replay/finish storage, hash density and witness bounds. No protocol or
finish changes are implicitly authorized here.

Oversized repeat buckets are a separate time risk. Cyclic periodic text can
have an arbitrarily large equal-key bucket at every finite N. Refining N cannot
solve it. A bounded exact selection fallback can repeatedly select the next B
suffixes using the existing total comparator, emit them and discard the segment.
It preserves the position bound and correctness but may be quadratic in n/B;
a production periodic fast path needs separate qualification. Hash claims in
Keys::cmp remain verified by actual corpus bytes, so no probabilistic ordering
assumption is introduced.

## Evidence and implementation

Measurement, selected N, source locations, gates and counters are recorded in
`tools/sol8a/RESULTS.md` when acquired. No 10GB or pile result follows from the
1GB measurements. The active 100GB run's scratch is never opened or modified.
