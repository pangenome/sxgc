# Design B measurement notes

Prototype: proto.cpp, C++17, own implementation. No repository files edited.
Host: AMD EPYC 7713, 256 logical CPUs, ~1 TiB RAM; single thread, unpinned,
shared host; no disk-backed index. Synthetic sizes decimal bytes.
Logical sentinel is symbol 0, text byte b is symbol b+1 (16-bit alphabet).
Text 0x00 and 0x05 never collide with sentinel. Benchmarks use the bytes given;
random full-alphabet data is already a bijective remap distribution; the real
snap is measured as raw bytes, not a claimed production-remap gate.

Structure: implicit randomized treap of packed 128-run blocks. Subtree and
local per-symbol uint64 aggregates, 16-byte run slots, sizeof(Node)=6248.
Per-byte operation: old C+rank; erase old tagged sentinel; insert text byte;
insert tagged sentinel at old-state calculated row. Global counts then update.
No construction SA/ISA or suffix-coordinate arrays. Input byte vector is
RAM-resident, explicitly outside the dynamic structure accounting. Global
C calculation scans up to 256 counters, included in timings. Structural merges
recompute 257 counts; ordinary edits update only the edited symbol along path.
Adjacent equal segments inside a block coalesce; equal segments across block
boundaries are logically coalesced by output/stats, but remain physical
fragments. Thus this measures a blocked run rope, not an ideal canonical
one-record-per-maximal-run implementation. Allocation totals include live
Node storage, exclude allocator overhead/input vector; RSS is in .time files.

Correctness: check binary compiled WITH assertions. Bench compiled -DNDEBUG.
10,314 tests: exhaustive lengths 1..8 over {0,5,30}; wrap-prefix {30,10,30};
periods 2/3/7 repeated 1..12 times, both with/without trailing 30; 300 random
web-like separator strings; 100 scattered 96-byte real windows (9600 bytes).
Each small input: build online sentinel BWT, compare every row to directly
sorted finite suffixes; LF locate every row independently against reference SA;
convert and compare full ascending-position cyclic order against direct sort.
Reference SA vectors exist only in this small test oracle. Conversion uses no
SA column: repeated LF from a row to known SA-zero sentinel row returns position.
Tagged padded/raw frame normalization and SA-zero predecessor rewriting checked.
Actual production RepairStream/container not invoked; byte marker handling and
remap-era banked reference compatibility are outside this attestation.
100 MB timing inputs are not brute-force sorted; correctness is small-input
oracle plus the argument below. Not an end-to-end production fixture gate.

Conversion algorithm and proof:
Enumerate each proper terminal suffix P=T[n-d..n); its finite-suffix prefix
interval contains all occurrences of P. Collect intervals with >1 row, retain
maximal intervals (prefix-tree intervals are nested or disjoint). Within each,
repeatedly select cyclic-key minimum greater than previous, using LF locate and
cyclic comparisons; tie on ascending corpus position. Leave other rows alone.
If two finite-suffix keys disagree before either ends, cyclic keys have the
same first disagreement. Therefore any pair whose relative order changes has
one finite suffix as prefix of the other. The shorter suffix is a terminal
suffix P, so both positions are in its prefix interval, hence in a retained
maximal interval. Reordering these intervals by exact cyclic comparator fixes
all and only possible inversions. Equal rotations also belong to such an
interval and use ascending-position tie rule. Delete the sentinel-suffix row;
rewrite the tagged SA-zero BWT entry to T[n-1]. Rebuild/coalesce predecessor
runs from converted positions, giving exact head/tail positions as well.
Byte 0 may precede a physical dollar byte 2 numerically; padding must remain a
logical tagged smallest symbol. This proof uses the audited finite-suffix +
logical-dollar semantics, not a literal byte-2 suffix sort on unrestricted text.

Conversion is deliberately slow: interval discovery repeatedly scans rows and
LF-locates, repeated selection is quadratic in interval cardinality; comparisons
can inspect n bytes. It certifies semantics, not feasibility at 100 MB/whole pile.
Conversion and finish are EXCLUDED from measured u. Serial q dependence prevents
trivial parallelization; independent chunks are a different algorithm.

Hardware perf denied: perf_event_paranoid=4. perf.err records failure; no settings
changed and no software installed. Minor/major faults from /usr/bin/time are
allocation/paging indicators, not hardware cache-miss counts. Pointer-tree paths
perform dependent random loads, three edit traversals per byte, plus leaf run
scans and occasional 257-counter structural updates. These predict cache/TLB
pressure but do not establish a measured hardware miss rate. Rank samples one
query every 4096 bytes, steady_clock bracketed, includes timer overhead, includes
small initial tree, aggregate mean/p99 is not a final-size-only distribution.

No contact_supervisor tool was present in ALL_TOOLS (searched supervisor/intercom).
No supervisor coordination was possible or required for the implemented scope.

Additional residual: block occupancy is not repaired by underfull-block merges;
live allocation may depend on historical run fragmentation, not just final r.
The final CLI bug (stoull attempted on real file path) was corrected before the
successful real measurement. Initial failure retained in real-cli-failure.time.
Constructor code used for earlier random timings is unchanged by the CLI fix.
