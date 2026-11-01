# Chi mmap bitmap qualification

Base: `6dd06ef6e141a03522c5929d1e411cca468c7c01`. Worktree:
`/home/erikg/chi-bitmap`; all build/gate scratch is under
`/mnt/nvme2n1/erikg/chi-bitmap`. The running 100 GB build and its scratch were
not modified. Native finish source and packaged runtime source are identical.

The change replaces the witness buckets/uint64 vectors/spill merges with a
file-backed, random-advised position bitmap. Existing codec 105 Elias–Fano is
retained, with bounded low/high output buffers. The bitmap scan verifies strict
position order, cardinality and uniqueness. Publication logs exact payload size.
No query reader or wire format change is involved.

The requested suffix-rank monotonicity assertion is deliberately **not** added:
the existing sweep emits ranks 3,2,0 on `banana+NUL`. Changing its representative
rule would not preserve today's chi. `rank-obstruction.log` records the existing
regression. Position monotonicity is the m5 contract and is asserted.

## Completed measurements

| Fixture | Chi | Bitmap bytes | Published m5 bytes | Bits/entry | Finish peak RSS (KiB) |
| --- | ---: | ---: | ---: | ---: | ---: |
| deterministic 8 MB | 6,544,643 | 1,000,001 | 1,818,105 | 2.222404 | 78,800 |
| 150 MB | 44,034,230 | 18,750,001 | 20,383,582 | 3.703225 | 276,268 |
| 1 GB prefix sanity | 283,296,724 | 125,000,001 | 133,324,206 | 3.764935 | 1,400,020 |
| 10 GB | 2,676,929,445 | 1,250,002,158 | 1,294,233,465 | 3.867815 | 13,403,192 |

X1 passes, including complete m5 byte identity, dimensions and CRC against
`recursive-planner/release-{8,150}mb/product.sxi2`. Identity JSON and complete
commands/timings live in the evidence root. X2 passes: 100/100 counts and all
574 positions match `tools/binbwt/oracle.tsv`. The existing query script ran
against the **new** 150 MB product.

X4 has a completed 1 GB run: peak RSS includes corpus, bitmap and the rest of
finish, and is below the 2,266,373,792-byte raw chi vector alone. mmap does not
make resident pages free. In the 10 GB process, sampled `/proc/PID/smaps` records
show bitmap pages as file-backed with zero anonymous bytes; samples are retained
in `retry-10/rss-samples.jsonl`. There is no chi-sized heap vector in this path.
No claim is made that total finish RSS equals bitmap size.

Additional checks pass: bitmap self-test (dense/sparse/empty, duplicates,
partial-byte boundaries, bridge first-emission order, disabled construction,
cleanup); sweep-state carry oracle (2,048 streams / 34,785 splits); existing rank
obstruction regression; optional-chi member identity, exact 8 MB query answers,
and missing-chi flag refusals. Native and packaged finish sources compare equal.

## Reproduction and caveats

Compile finish with C++17/O3/pthreads, vendored sdsl headers and `ram_fs.cpp`,
plus libatomic. Compile the **packaged** sorter at
`xsa/runtime/tools/binbwt/scatter_sort.cpp`; the historical top-level copy lacks
current `--budget-bytes` options. Compile `bit6/cross_lcp_merge.cpp` with C++17,
O2, mcx16, pthreads and libatomic. Exact binary hashes: `binaries.sha256`.
All successful builds use `tools/binbwt/optional_chi_gate.py --with-chi`;
each run directory records its complete three-stage `commands.json`.

The removed deterministic 8 MB source fixture was reproduced from the original
`/tmp/tape-smoke.py` recipe, entirely in our own scratch:
`random.Random(8).randbytes(8000000).translate(bytes(3+i%247 for i in range(256))) + b'\x1e'`.
Its exact baseline m5 comparison passes. An additional 8 MB text-prefix fixture
also has exact baseline m5 identity (chi 2,451,384); it is not substituted for X1.

Initial attempts with the historical sorter failed on its unsupported CLI.
The first production-sorter 10 GB attempt (96 threads) failed its existing
completed-band row bound (8,231,294 rows vs target 4,809,757). The retry uses
32 threads, the same 48 GiB sorter envelope and 16 pending bands. No sorter
source or row-bound assertion was changed to bypass that failure.

X3 passes: the 10 GB pipeline completed in 1500.85 seconds, with all three
stages exiting zero. Chi is exactly 2,676,929,445. Entire m5 bytes, dimensions,
CRC and SHA-256 match `r10g-m96/pile-10b-snap.sxi2` (SHA-256
`f9dee338665f64a21d12fe0c46f6ec62f2bd98cc7fcf764fe06365b1dab83f1e`).
The query gate passes 20/20 low-count patterns, counts **and** positions, against
`/tmp/g10/oracle.tsv`. Query wall time was 22.43 seconds; its separate RSS
(34,294,096 KiB) is not the finish RSS. Evidence: `identity-10.json`,
`query-10.log`, `validation-10.json`, `retry-10/{commands,result}.json`, and
`retry-10/finish.{log,time}`. All successful builds remove their bitmap files.
The 10 GB raw chi array would require 21,415,435,560 bytes; none is allocated.

X1–X4 pass, satisfying the qualification condition for X5. Machine-readable
results, all journals, commands, RSS samples and binary hashes remain in the
scratch evidence root. Native/packaged source identity and `git diff --check`
also pass. The branch can be fast-forwarded remotely without updating the
working files of the concurrent 100 GB build.

The existing m13 u32 run-index limit is unchanged. This qualification does not
claim a completed 100 GB build or removal of that separate publication limit.
