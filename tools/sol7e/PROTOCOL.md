# Fresh-start producer transport and accounting

`CROSS_V2_PRODUCERS` selects S (default 1, range 1..4096, clamped to rows).
`CROSS_SHARDS` still selects legacy emission sharding. The pipeline explicitly
passes and journals the new knob. S=1 uses SequentialV2 directly, preserving
the existing walk. S>1 uses P independent fresh heaps and compressed segment
spools; there is one ordered reducer and one canonical BPG1/BPT1 publisher.

Segments are `(char,len,head,tail)`. Every discovered seam target is a singleton
segment. All other rows can be coalesced without losing class occurrences or
flanks: the reducer consumes the head normally, advances over non-target rows,
and retains the tail as the next row's predecessor. This preserves continued
runs and classes across empty or repeated range cuts. The native production
oracle compares the actual emitted columns and class entries with dense output.

A producer publishes a committed prefix after each complete 4096-record write.
The reducer drains prefixes before local EOF and validates contiguous row
coverage before advancing to the next producer. Only all ranges plus complete
class/flank validation permit global terminal. Input-dependent scratch is at
most 25*n bytes, with P*4096*25 bytes of producer buffers, plus one reducer
buffer. There is no bounded per-producer queue blocking later producers behind
the first range; their remaining output occupies scratch. This differs from
the existing canonical-band-only model. There is no durable worker retry or
process-recovery claim. Failure terminates the process and refuses terminal.

At P=16,k=8, decoded front windows are bounded by 4,194,304 bytes, shared across
no producers (each producer owns its windows). Corpus bytes are immutable and
shared. cache_oracle.cpp tests concurrently retained borrowing spans.

The three physical logical-work metrics stay separate: comparisons, symbols,
probes. `WALK` sums producer heap work plus the unchanged parallel prepare/repair
work, so P=1 equals the existing whole-merge counters. The `heap` and `prepare`
stages also report the two portions separately.
`PLANNING` records rank searches. `OVERLAP` is zero: no row is re-emitted.
`SEAM` is the SIGNED producer-minus-serial-range delta, including the different
heap exhaustion transitions at the range end. It is not generally positive or
localized to the beginning of a range. Adding SEAM to WALK would double-count
it. Whole charged work is PLANNING + WALK, equivalently PREPARE +
SERIAL_RANGE + PLANNING + OVERLAP + SEAM. At P=1 all extra categories are zero.

`CROSS_SEAM_ORACLE=1` enables an explicitly labeled diagnostic serial replay
without seeded producers. Its reference trajectory is independently cross-
checked against sol7d's seeded producer budgets at all 40 fixtures/modes/P.
Diagnostic replay work is reported as `oracle_reference`, not disguised as
production planning; diagnostic walls are not launch walls. With
`CROSS_SEAM_REFERENCE=/absolute/path`, an oracle run writes the per-range
reference once; later timed runs read it without replay. The journal binds
mode, hash spacing, every split, source paths/inodes/sizes/mtime, segment
ranges and remaps. It requires immutable inputs: metadata-preserving malicious
modification is outside this contract. Without a reference, P>1 reports SEAM
as unmeasured, never zero. Source journals should be regenerated after changing
the algorithm (format version sol7e-range-v1).
