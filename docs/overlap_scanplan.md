# Cold scan-only census (10 GB calibration gate; 100 GB refused)

No product mode. The parent schedules at most one calibrated study; nothing
here dispatches another stage. Tiny tests in a worktree, no real raw input:

```sh
t=$(mktemp -d)
cc -std=c11 -O3 -Wall -Wextra -Werror -fPIC -shared gates/overlap_scanplan_chunk.c -o "$t/chunk.so"
python3 -BE gates/overlap_scanplan_test.py "$t/chunk.so"
python3 -BE gates/overlap_scanplan_gate_test.py
bash gates/overlap_scanplan_gate.sh 100g 0   # must refuse, exit 75
```

Parent CLI **only if separately scheduled**:
`bash gates/overlap_scanplan_gate.sh 10g DEADLINE_UNIX`. Linux, compiled
committed pinned source, systemd user dedicated cgroup (192 MiB total,
100% quota, no swap), same nonblocking `overlap-ten-timing.lock` as product
runs. Full pinned historical `packed-serial-10g` receipt is replayed by
`overlap_ten_matched_score.metrics` before touching raw bytes; a front-only
receipt is insufficient. Build tiny C helper; replay brute fixtures in the
same grant. Fresh mode-0700 private directory, fixed source and exact size,
non-reflink sequential copy, SHA during copy, fsync/evict and mincore all
pages on copy, compare independent SHA from scan. Trust source as immutable;
stat before/after. No historical 10 GB raw hash is inferred from the product
SHA. Fail if any of n, m, starts > UINT32_MAX, alphabet size, or longest raw
phrase disagrees with the pinned 10 GB front census. Only a complete receipt
`scan.json` with `CALIBRATED_10G_SCAN_ONLY` and normal exit is useful.

**Costs/guards.** Copy costs N logical read + N write; scan exactly N logical
read in byte order (plus EOF check), ~1 MiB chunks, at most eight carry bytes,
256×257 u64 counters, five u64 trigger counters, streaming digest, no raw
mapping during scan. Time O(NW + cells), W=8 fixed. No phrase strings,
columns, scratch runs, dictionary, dedup or SA. Private copy (N), compiled
helper, bounded sparse census/plans/receipts are not counted column scratch
(0). Reserve N + 1 GiB free before copy. Pre-receipt whole-dir allocation is
separate. Input copy, scan and setup aren't a raw-to-product or prepared wall.
Absolute epoch deadline and private `stop.requested` polled each chunk; phase
worker /proc/self/io and unsummed per-device io.stat deltas plus disk free
polled at least every 0.25 s at chunk boundaries. Undelegated io.stat is null;
worker read/write deltas required. Copy/cold caps N+64 MiB each direction;
scan caps N+64 MiB read, 64 MiB write and requires nonzero cold worker reads.
Cooperative, not a hard IO limit or kill guarantee while blocked in kernel
IO. No pass inferred from an OOM, incomplete receipt, or passing cap alone.

**Census semantics.** Same wrapping u64 FNV-1a constants as lexparse10;
test every window starting at `1..N-8` inclusive for `hash % 31 == 0`.
Word starts at 0 and at each trigger. Previous word ends after the trigger's
eight bytes, owns just the stride to next start. Final word always owns
through N (eight bytes even on final trigger); report EOF start/length.
Ownership partitions the raw byte positions. Therefore each non-last owned
position has the *same* first two finite symbols as the adjacent raw pair,
including across cuts. The last row begins `(final byte, END)`. Sparse cells
are `[first_raw*257+second_code, occurrence_rows]` where END=0 and raw
seconds use 1..256. Order is equivalent to an order-preserving byte remap;
no packed 8-bit encoding or two spare codes required to measure it. Global
END is a separate one-row class, not in the histogram. Sum cells=N.

**Arithmetic-only planning.** Let k be distinct words, U the sum of their
owned lengths. Each occurs at least once, so k≤m and U≤N. k-1 overlaps of
eight bytes plus k separators and one END give D=U+9k-7≤N+9m-7. Packed
tape of four-byte header plus raw payload per distinct word costs
D+3k-1≤N+12m-8. Python exact arithmetic with u64 output extent checks.
No point estimates of k, D, U or any descriptor cell load. For each cell
occurrence mass dominates its descriptor count (positive word frequency).
Greedily accumulate adjacent whole nonempty cells subject to mass ≤ arena/8;
report inclusive first/last keys and mass upper bound per range. Single
oversize cell means no such plan, not proof a dictionary won't fit. Compact
width admission conservatively requires k upper bound ≤ u32 and encoded word
extents ≤ u32. Arenas 256 MiB / 1 GiB are **hypothetical descriptor payloads**,
not the scanner grant or a product default. Worst-case descriptor payload
≤8N. For a preallocated 20-byte compact cursor per list in a single equal
finite-tail group, membership ≤ largest cell mass and ≤ k (different offsets
of one word have different lengths). Reserve `20*min(m,largest_cell)` bytes
*in addition* to the tail arena; no dynamic heap growth assumed. This does
not account for dictionary, occurrence lists, histogram/plan, RMQ, allocator,
control or consumers. No range/heap bound certifies end-to-end fit, speed,
physical IO savings, or a rank/runtime result.

**100 GB seam: HOLD, exit 75 before any access.** No 100 GB worker, path
argument, authorization override or automatic scale-up. Future narrowly
reviewed admission must take a committed receipt manifest, not a caller's
`selected=true`: replay `overlap_scaling_policy.evaluate(control,pair,grid,
arms)` on exactly the three full matched 10 GB arms in declared order,
require complete sweep, non-null eligible healthy selected row, verified
identity/row counts, stops/exits/OOM/grants/scratch/IO and exact profile
comparisons; bind all receipt hashes, selected settings and documented
measurements/decision to a clean commit verified reachable from freshly
checked `origin/main`. Replay matching calibration code/fixture, banked
fields/SHA, cell sum, bounds, cold/grant/IO/stop evidence from published
10 GB scan receipt. Require pinned 100,000,000,000-byte manifest raw SHA
`de79fe96057b604a71513770111f246ab0e936d3cf56415bcc5a43bba60bbbfd`,
fixed source, private-copy space, identical isolation and guards, actual
remaining absolute time covering copy+cold+scan+receipt with explicit
margin based on measured calibration and IO conditions. Fail on any missing
or stale evidence. Only then could parent dispatch a separately reviewed
scan-only 100 GB entry (with SHA comparison and width checks); no product,
larger dictionary phase, or throughput conclusion follows. This tree does
not contain or fabricate the required complete published decision bundle.
