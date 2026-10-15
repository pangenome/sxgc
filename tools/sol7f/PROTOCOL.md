# Flow-bounded compressed segment transport

`CROSS_V2_SPOOL_BYTES` is an unsigned decimal byte budget. Default: max(2n,
P*106496); explicit budgets smaller than P*106496 are rejected. P=1 retains
the direct serial sink and writes no segment spools. No corpus text is spooled.

Producers reserve shared credits before writing each 4096-record batch (maximum page charge 106496 bytes). Charges round up to filesystem pages (4096 bytes); the effective budget
rounds down to whole batches. One batch is reserved for the currently ordered
range: other producers may consume at most budget minus that batch. This avoids
the classic deadlock where later ranges occupy all credits while the reducer
needs an unpublished prefix from the earliest range. Empty ranges can complete
without credits. The active range advances only after contiguous coverage and
local EOF validation. Condition predicates and reservations use the same mutex.

The reducer reads only committed prefixes, emits complete segments through the
unchanged canonical publisher, punches consumed file extents with KEEP_SIZE,
and only then returns credits and wakes producers. Every nonfinal batch is
page aligned; the final partial batch is rounded up and has no successor.
Logical file lengths can grow to 25n, but allocated data pages remain within
the credit bound. Filesystem metadata and directory blocks are additional.
Unsupported hole punching fails closed. There is no fallback that silently
permits unbounded physical spools. All spool fds are closed and names unlinked
at local EOF. Global terminal still requires all ranges, class/flank validation
and canonical publication. Existing W=1100 ACKs are independent of upstream
completion; a downstream stall intentionally propagates to producers.

`CROSS_SPOOL_STALL` records per-producer wait episodes and elapsed wait seconds
(sum across threads is not wall time). Existing WALK, PLANNING, OVERLAP and
signed SEAM accounting is unchanged; credit waits perform no comparisons.

Before the transport implementation changed, the threaded model passed 400
schedules. Its final stronger version additionally forces W=1100 exhaustion,
checks exact-at-budget admission, zero final outstanding credits, empty ranges,
terminal-only-after-coverage, randomized producer starts, and multiple P=8
completion orders. The model tests liveness assuming eventual independent ACKs;
it does not claim recovery from a crashed consumer or durable worker retries.

XFS discovery: ordinary extending pwrite may speculatively allocate EOF extents
well beyond its byte count. An initial 1GB run was stopped when du showed this
violated the intended physical bound despite correct logical credits. The final
implementation first fallocates exactly the credited page-aligned write extent;
pwrite then converts already allocated pages. The scratch-filesystem oracle
checks 2000 extending writes with an eleven-batch live budget, including punch
and allocation, using st_blocks rather than apparent length. The initial run is
retained as an aborted diagnostic, not a qualified performance measurement.


The first strict-credit 1GB trial crossed 320 seconds before completing, so
backpressure alone did not meet the wall bar on this shared box. The final
transport also packs each batch losslessly: character byte, unsigned LEB128 run
length, n-width endpoints, and an omitted duplicate tail for singleton segments.
An eight-byte header binds payload bytes and record count/codec. Incompressible
batches fall back to the original 25-byte records. Every batch occupies an
independent fixed-address 106496-byte logical slot, allocating only its rounded
payload. This preserves constant metadata in RAM and supports committed-prefix
reads without an ever-growing offset index. The reducer validates framing and
restores the identical segment tuples before canonical publication. The codec
oracle covers widths 1..8, full batches, partial batches, UINT64_MAX lengths,
singletons and raw fallback; native column/class and byte gates exercise it.
The 400-schedule credit proof is unchanged: a batch's page charge is known
before admission, and the active range retains one maximum-batch reserve.
