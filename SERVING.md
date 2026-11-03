# Batched next-byte branches

```
xsa branches corpus.sxi2 --contexts contexts.txt -k 8 --jsonl
xsa branches corpus.sxi2 --serve -j 32 -k 8
```

Both modes use the existing block-compressed SXI2 v6 reader (m1 codec 102 or
114); older codec-101 containers require rebuilding. Both return exact next-byte
frequencies after the **entire context**.
Overlapping occurrences count separately. Matching uses the container's byte
permutation and backward search; returned bytes are original corpus bytes.
No byte is used as an absent-result sentinel. The corpus is a single byte
sequence: document separator 0x1E is an ordinary searchable byte.

This v6 lite container has no move/psi member. Its sparse m11 anchors contain
the SA coordinate at the tail of every 1024th BWT run. For intervals of at most
64 rows, this implementation walks LF to an anchor for each occurrence, then
reads `corpus[SA[row] + context.len()]`. Cost is O(context length rank steps +
sum of LF walk lengths + width corpus gathers), **not** O(width) constant-time
moves. There is no guaranteed short LF walk. The reader's existing materialized
rank columns and row directory are shared across workers; each worker reuses a
private navigation context.

For wider intervals, exact backward searches of `context || byte` compute
right-extension counts for each present byte. This avoids millions of sparse
LF walks for common and one-byte contexts. It is not `rank(byte,r)-rank(byte,l)`,
which would count preceding bytes. The empty context uses corpus byte totals
and occurs at all `corpus_bytes + 1` boundaries, including EOF.

The fallback needs the original corpus. Matching document sidecars and
`corpus.bin` are discovered beside the container or from its build journal.
Use `--docs DIR` to specify them, or `--corpus FILE` for a corpus without document
sidecars. Length must equal the container's `n-1`. Explicit corpus files must be
from the indexed corpus; equal length alone does not establish identity.

## File mode

`--contexts FILE` reads newline-delimited byte contexts, assigning zero-based
`ctx_id` values. A blank line is an empty context; a terminal newline does not
add another context; an empty file contains zero contexts. CR bytes are preserved.
Use framed mode for contexts containing newline bytes. `--jsonl` emits one
object per context with `ctx_id,l,r,width,eof,other,branches`, where each branch
has numeric `byte` and `count`. Default TSV fields are those six scalar fields
followed by the list of `(byte,count)` pairs. File mode processes sequentially.

Branches sort by descending count, then ascending byte. `-k N` retains at most
N branches (0 through 256; default 256). A zero-count byte is never emitted.

For every response:

```
width = r - l
sum(emitted branch counts) + other + eof = width
```

`eof` counts occurrences ending at the corpus end, with no following byte.
`other` counts omitted branches, and is zero without truncation. Empty intervals
have width 0, eof 0, other 0, and no branches. An EOF-only context has
`eof == width` and no branches. These meanings also hold for empty contexts.

## Streaming protocol

`--serve -j N` starts N workers in one process; N must be positive. stdin and
stdout are binary pipes. Diagnostics go exclusively to stderr. Each response
is flushed immediately, without waiting for EOF or a full batch. Pipelining
requests keeps workers busy; clients should read responses while sending.
Outstanding requests and the ordered result buffer are bounded to 4N requests
(plus one request being read). Default output order is request order.
`--unordered` permits completion order; `ctx_id` always identifies input order.

All integer fields are unsigned little endian, with no padding:

| Request field | Bytes |
| --- | ---: |
| Context byte length | 4 |
| Context bytes | length |

Context bytes are verbatim, including leading `-`, 0x1E, NUL, 0xff and newline.
Zero-length requests are valid. Requests above 16 MiB are rejected before
allocation. EOF between frames closes the stream normally; truncated headers
or bodies produce a nonzero exit and stderr diagnostic.

| Response field | Bytes |
| --- | ---: |
| Payload byte length (excludes this field) | 4 |
| ctx_id | 8 |
| l | 8 |
| r | 8 |
| width | 8 |
| eof | 8 |
| other | 8 |
| nbranches | 2 |
| Repeated: byte, count | 1 + 8 each |

Payload length is `50 + 9*nbranches`. IDs start at zero for each process.

`BRANCH_READY` marks completion of index loading. `BRANCH_STATS` reports
post-load throughput, mean backward-search time (including right extensions),
mean gather time (including LF walks and branch sorting), output serialization
and pipe-write time, request-read time, and total LF steps. Kernel times are summed across workers,
so their sum is not wall latency at N > 1. Input and output IPC timings include blocking
reads/writes (including idle time waiting for a client); client end-to-end throughput also includes request transport and
scheduling. Loading the index is outside the warm benchmark timing.

## Reproducible gates

`tools/binbwt/tests/branches_gate.py XSA CONTAINER CORPUS --out DIR` computes
100 distributions independently using Python `mmap.find`, checks full and top-8
responses, seven edge cases, and count/position regressions. The 150 MB gate
uses the existing banked 100-pattern oracle; the 10 GB gate checks the existing 20 low-count
patterns against their banked positions. `--reuse-oracle` replays a previously
computed independent branch oracle without rescanning corpus bytes.

`tools/binbwt/tests/branches_bench.py XSA CONTAINER CORPUS --jobs N --out DIR`
pipelines 100,000 contexts after 1,000 warmup requests. The deterministic mix is
90% sampled 48-byte contexts and 10% common English contexts. It checks frame
order and the closed count invariant on every reply.

## Measured gates (2026-11-03)

W1: **100/100** exact distributions, EOF and width at each of 150 MB and 10 GB.
Top-k closure also passed 100/100 at each size. W3 passed 7/7 edge contexts at
each size. File JSONL passed 12/12 at each size. W4 retained the existing
100/100 and 20/20 count/position gates, including unchanged `--count` values.
Query-kernel unit tests: 18 passed.

100 GB container, k=8, 100,000 requests after 1,000 warmup requests:

| Workers | Contexts/s | Search µs/context | Gather µs/context | Input IPC µs/context | Output IPC µs/context | LF steps/context |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 104.664 | 321.562 | 9224.533 | 1.601 | 7.965 | 31744.8 |
| 32 | 553.836 | 424.870 | 10196.409 | 3.689 | 2.190 | 31744.8 |

The 20,000 contexts/s target was **not met**. Profiles include warmup;
throughput excludes warmup and loading. IPC times include blocking reads/writes
and are not additive with parallel kernel times. These are shared-host
measurements, not an isolated-machine capacity guarantee. The existing reader
materializes about 266 GB of index columns for this corpus.

Raw gate logs, independent oracles, exact container/corpus paths, binary hash,
and benchmark results are in [the evidence directory](tools/binbwt/evidence/branches/results.json).
