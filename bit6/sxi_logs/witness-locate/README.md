# Witness locate lane

The proposed fixed χ-row bitmap is not a complete sample set for exact
find-one queries.
`lean/Sxgc.lean` defines χ members as context **ends** and
`covering_given_stream` proves coverage for requirements `(w,c)` with
right-maximal `w`. It does not claim that every arbitrary pattern interval
contains a χ row. The independent read-only `audit.cpp` maps all χ members to
run-edge rows and checks the retained query sets. All χ members mapped, but
many occurring patterns had no χ row in their interval:

| Corpus | Occurring of 1,001 | Nonempty intervals without χ |
| --- | ---: | ---: |
| yeast235 | 871 | 391 |
| pile-frag | 779 | 425 |

The frame matters: χ records context ends. A pattern ending at χ position
`x` starts at `(x - |P| + 1) mod n`; the SA row for that start depends on
`|P|`. A fixed bitmap marking the rows corresponding directly to χ ends
cannot represent all those starts. The audit tests that proposed fixed-row
bitmap, not whether the formal covering theorem holds.

Each query set includes one `gate` pattern and 1,000 `probe` patterns. The
yeast gate pattern is among the missing intervals. Thus for the 1,000-probe
workload the yeast count is 390/870. The pile gate has a witness, so its
count is 425/778. See `*.audit.json` and `*.audit.log`.

The implementation keeps the χ route where it applies. `witness-build`
streams EF χ into a temporary position bitmap, maps every member through
compact φ edges, and writes a separate sidecar. `mems --first` uses backward
search, rank/select on the two edge slots per run, and stored φ recovery.
When an occurring interval has no χ member, it uses the existing search
toehold, which recovers one SA value through run-head φ. Both routes avoid
enumeration and LF walks. The output's `source` field identifies the route.
`--verify-first` is a gate option that deliberately invokes LF-based locate
for the returned row and rejects any coordinate mismatch.

This sidecar needs **two bits per run** because an individual run can have χ
members at both head and tail. A single R-bit bearing-run bitmap loses which
edge (or both) carries the member. The file contains 64 header bytes plus
ceil(2R/8) bytes. The runtime adds one 64-bit rank count per 512 slots.
Route A needs no Elias–Fano witness-position array; the existing φ map supplies
the position. A hypothetical 12-bit/witness array would cost ceil(12χ/8)
bytes. Witness positions in BWT row order are not monotone, so ordinary
Elias–Fano cannot directly encode that array; route B would also need a
permutation or another sequence encoding.

| Corpus | Artifact bytes | R/8 ideal | Actual sidecar | Runtime rank + next block | EF if 12 bits/χ |
| --- | ---: | ---: | ---: | ---: | ---: |
| yeast235 | 1,413,689,216 | 12,613,131 | 25,226,326 | 4,729,944 | 128,106,504 |
| pile-frag | 5,196,757,368 | 49,715,377 | 99,430,817 | 18,643,284 | 459,247,148 |

The actual sidecar includes a 64-byte header. Runtime overhead is the
8-byte/512-slot rank array plus a 4-byte/512-slot successor block array,
including each array's sentinel.

The sidecars in this directory were built from the retained SXI2 files at
`/mnt/nvme3n1/erikg/sxi2-v5/`; those files were read only. The builder and
query code never read source corpus bytes. `check.py` reads each source corpus
once, forwards through sorted returned coordinates, and compares actual
context bytes. The gate inputs are the first 1,000 `probe` rows from
`../v7-model/{yeast235,pile-frag}.queries.tsv`.

## Correctness gate

`mems --first --verify-first` checked every returned row against the existing
LF-based locate path. `check.py` checked every returned context against the
source bytes in a single forward pass and compared occurrence presence with
the existing sampled locate path:

| Corpus | Pattern decisions | True occurrences checked | No occurrence | χ route | Toehold route |
| --- | ---: | ---: | ---: | ---: | ---: |
| yeast235 | 1,000/1,000 | 872/872 | 128 | 478 | 394 |
| pile-frag | 1,000/1,000 | 778/778 | 222 | 127 | 651 |

See `*.check.json`, `*.first-verified.jsonl`, `*.first-verified.log`, and
`*.sample.jsonl`. Every fixed-row χ miss is covered by the no-LF toehold
route; the proposed fixed-row completeness gate itself **fails** as shown
above.

Example invocation (from the repository root):

```sh
xsa/target/release/xsa witness-build --sxi /mnt/nvme3n1/erikg/sxi2-v5/yeast235.sxi2 --output /tmp/yeast235-new.wit
xsa/target/release/xsa mems --first --sxi /mnt/nvme3n1/erikg/sxi2-v5/yeast235.sxi2 --witness-index /tmp/yeast235-new.wit --reads bit6/sxi_logs/witness-locate/yeast235.fa --mode dna
```

The first `--verify-first` run rejected a coordinate computed as `n-1-SA`.
That was an off-frame error: φ and the search toehold already contain the
indexed text position, while `s_at_opt` contains its mirrored value. The
implementation now passes the φ/toehold position directly to `annotate`.
The old failure log `yeast235.first-verify.log` is retained; the final
`*.first-verified.log` runs pass.

The production path uses the search toehold directly for singleton
intervals. This makes the common one-occurrence case search-cost-only;
multirow intervals try the χ bitmap and use the same toehold if χ is absent.
The route counts above include this singleton shortcut. They therefore differ
from the pure-χ misses in the independent audit.

## Warm speed gate

`bench.py` sends the same 1,000 probe patterns to one worker over HTTP after
10 warmup requests. Startup and sidecar construction are excluded. Every
response body is fully read. `full` enumerates all occurrences; `first`
returns one; `sample` uses the existing one-position locate path.

| Corpus | First median | Full median | First/full | Existing sample median |
| --- | ---: | ---: | ---: | ---: |
| yeast235 | 0.513 ms | 5.671 ms | 0.091 | — |
| pile-frag | 0.369 ms | 0.410 ms | 0.900 | 0.340 ms |

The fragment result is 0.009 ms (2.5%) above the cited v6-2 warm 0.360 ms
reference, which used a different single pattern. It is faster than full
locate on the identical 1,000-pattern workload. This does **not** meet a
literal ≤0.360 ms gate. See `*.time.json` for mean and p95 as well. The
390 MB yeast full-locate response is retained as `yeast235.full.jsonl.gz`.
