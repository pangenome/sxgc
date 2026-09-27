# Phi-inverse head sample extractor

The revised tail-only front-end construction is unresolved; see
[SXI_CONSTRUCTION_BLOCKER.md](SXI_CONSTRUCTION_BLOCKER.md). The
`--from-front-end` mode fails explicitly before file access. The documented
pilot route below remains operational and must not be reported as a fresh
build. SXI container and loader details are in [SXI_FORMAT.md](SXI_FORMAT.md).

`phi_inverse_heads.cpp` reads the immutable pilot index's O(r) Phi table.
It seeks past F, Psi, and intAtTop, and never reads PLCP samples. It does
not build or traverse LF, visit text positions, or consume an aggregate.
The supplied `.ri4` is read only for its header and packed tail samples.

For each source interval `[start,end)`, the serialized move table stores
an output interval number `q` and offset `d`, so `dest = start[q] + d`.
The tool sorts `(dest,start,end-start)` in place, checks that both source
and image intervals partition `[0,n)`, and answers each inverse query by
binary search over `dest`. For run zero, the previous tail is run R-1.
`.ri4` samples use the mirrored convention: `tail = n-1-sample`.

Time is O(r log r + R log r), space is O(r), with a bounded output buffer.
Interval decoding is parallel; invalid intervals are reduced into a failure
count before sorting. Sorting uses bounded-depth parallel partitions and `std::sort` leaves;
it allocates no second full image array. At 466 the image array is
65,753,684,064 bytes and the packed Phi table is 33,561,776,256 bytes.
The index mapping is released before sorting and querying. The maximum
planned data footprint is about 99.32 GB, plus small buffers/metadata.
The input table's size is checked against a 150 GB budget before allocation.
Partial output is published by rename only after every query succeeds.

Build and test:

```sh
mkdir -p /tmp/laneS
g++ -O3 -std=c++17 -Wall -Wextra -fopenmp bit6/phi_inverse_heads.cpp -o /tmp/laneS/phi_inverse_heads
python3 bit6/test_phi_inverse_heads.py /tmp/laneS/phi_inverse_heads
bash tools/build_slim_dump.sh /tmp/laneS/slim_dump
cargo build --release --manifest-path xsa/Cargo.toml
```

Yeast recipe (output paths must be fresh):

```sh
/usr/bin/time -v /tmp/laneS/phi_inverse_heads \
  /mnt/nvme3n1/erikg/sxgc-yeast/grl/yp2t.lcp_index.lcp_index \
  /tmp/laneQ/pass3/yp2real.ri4 /tmp/laneS/yp2.phi.head_sa 64
python3 bit6/phi_inverse_check.py column \
  /tmp/laneY/yeast_pfp2.agg /tmp/laneS/yp2.phi.head_sa
/usr/bin/time -v /tmp/laneS/slim_dump \
  --ri4 /tmp/laneQ/pass3/yp2real.ri4 \
  --parse /mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast2_pfp \
  --slim --resolve-ri4 --head-sa /tmp/laneS/yp2.phi.head_sa \
  --dict-stream -t 64 -o /tmp/laneS/yeast.phi.agg
xsa/target/release/xsa chi-rspace --stream-agg \
  --ri4 /tmp/laneQ/pass3/yp2real.ri4 --agg /tmp/laneS/yeast.phi.agg \
  -o /tmp/laneS/yeast.phi.sA
python3 bit6/phi_inverse_check.py sets /tmp/laneS/yeast.phi.sA \
  /mnt/nvme3n1/erikg/sxgc-yeast/grl/chi_yeast_pfp2.sA \
  85404240 /tmp/laneS/yeast-sets
```

The aggregate is used by the `column` oracle check only; it never supplies
construction inputs. The new aggregate's comparison with the old one is
a reported diagnostic, not the end-to-end acceptance gate. The supplied
legacy `.ri4` is explicitly authorized by this task; it is never regenerated.
No claim is made that this lane constructs the front-end artifacts from raw
text. It constructs all downstream artifacts anew from the named inputs.

`phi_inverse_pipeline.py` resumes after the logged G0 dump and enforces
G0 chi/witnesses, G1 complete byte identity, then G2 extraction/anchors.
It stops on a required gate failure. If the 466 producer is active or its
parse is absent, it stops after G2. If the parse becomes available, it
requests local inspection of completion and memory requirements by exiting
with status 3; it does not launch an unbudgeted 466 dump.
`--resume-g1` resumes from the successful yeast end-to-end logs, requiring
the revised parallel extractor's yeast output to match the original head
input exactly. The initial serial-decoder k10 attempt was intentionally
terminated before producing heads; its timing is retained separately in
`phi_inverse_logs/G1.serial-extract.aborted.log`.

## Front-end `.ssa` audit (report only)

The pilot producer is `/home/erikg/r-pfbwt/include/rpfbwt_algorithm.hpp`,
called by `bit6/pfpbwt_pilot_k10.sh`. `pfpds::long_type` is `uint64_t` in
the dependency's `pfp/utils.hpp`.

- Lines 772–783 write one native-endian u64 count followed by that many
  native-endian u64 SA positions, concatenated in run-head order. On this
  little-endian machine the format is `[u64 R][u64 headSA × R]`.
- `h10ss_pfp.ssa` has count **1,859,825,862** and exact size
  **14,878,606,904 bytes = 8 + 8R**. It contains heads only, no row/value
  pairs and no tails. Positions use the producer's circular/padded text
  convention (adjustment at lines 661–665); they are not mirrored ri4
  samples. This single-string pilot has different run count/conventions
  from the newline-collection k10 chain and is not used as its oracle.
- Lines 668–670 already have `out_sa_value` and emit it at a head. Emitting
  HEAD+TAIL is a small, localized front-end sample-output change: select
  heads **or** tails (a tail precedes the next head, with final-row handling),
  preserve singleton/chunk-boundary semantics, and write the two fields.
  The range-selection check around lines 568–570 must also select ranges
  containing tails, including those with no head; changing only the final
  `if` is insufficient. The container writer must normalize padding and
  terminators and convert the tail convention when creating v5 fields.
- This inspection supports the ledger's small-change prediction for
  endpoint emission during construction. It is not an implementation or
  performance gate of that front end, and does not imply that its existing
  internal enumeration is an O(r) downstream algorithm. No producer source
  was changed or rerun.

## Workspace provenance

The actual checkout reports `03e3773`, not the task's stated `1cfef7d`.
Its working-copy `RESEARCH.md` ends at “THE HUMAN RUNG IS GREEN” and does
not contain “CORRECTION #7 + THE LAW”. The missing section was subsequently
located and read with `git show 1cfef7d:RESEARCH.md`; it confirms the explicit
task instructions. The ledger's approximately 30 GB extractor estimate is
not a measurement of this implementation; this implementation uses a wider
validated image table and is budgeted below the task's 150 GB ceiling.
The requested `contact_supervisor` tool is absent from the available tool
catalog. No commits are made and no unrelated processes are stopped.
