Single-pass v2 scratch protocol

The final merge attaches SequentialV2 directly to core_merge_kway, with
one ordered heap traversal. ParallelSparseRuns is deleted. SparseRunSpool remains
only for the standing legacy sparse-walk oracle; the final v2 count/replay
pair and immutable shard bases are retired. Legacy intermediate WM walks
and their shard plan remain supported.

BPS1 start: magic, n, packed width, discovery depth, window (u64 fields).
BPG1 progress: magic, zero-based band, run count, reference count, completed
class count; references are class ids. Each completed entry contains id,
first/last global raw run, followed by the SCS1 class fields and occurrences.
Band bases are exactly 65536*b. PRC1 scratch pieces retain their existing
binary header with shard field fixed to zero, one file per band.
BPT1 terminal: magic, final band N, R, captured rows, completed classes.
All files close before atomic rename. Progress follows piece publication.
Terminal follows complete target/flank validation and final BWT metadata.

The reader follows markers until intersecting class ids are complete, then
uses their actual largest run-band extent E(b). It waits for b+E(b), clamped
at N when the terminal marker is present. It validates aggregate repair
limits against final R. It unlinks markers as read and pieces as consumed;
C advances after repair and LCE/sweep processing. W defaults to 1100; the
producer admits band k only at k<=C+W, with C=-1 initially. An unclosed class is refused at its band boundary before its
next-band extent would exceed W-1; actual extents are checked again at EOF. CROSS_V2_BAND no longer changes the fixed size.

Test-only stall hooks: CROSS_V2_PRODUCER_DELAY_MS before each band publish,
PILE_FINISH_BAND_DELAY_MS before each consumed marker. Scratch directories
are removed on both sides at the existing cleanup points.
