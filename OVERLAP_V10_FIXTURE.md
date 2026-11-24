# Small exact-v10 fixture (qualification, not a scale gate)

**Rolling producer → complete v10 index:** On a new generated input, the rolling producer and direct Rust trie pipe built an index **byte-identical to all 378,392 bytes** of the independently constructed band-fed reference. Both passed the raw-row and document-boundary checks. [Result, costs and two preserved supervisor failures](gates/receipts/rolling-v10-product/). This is an end-to-end integration result, not a corpus benchmark.

**Earlier band-relay fixture (different input):** all 8,847,248 bytes match, including the directory and provenance.
The candidate consumes overlap-produced rows, not reference SA/BWT/LCP or trie
bytes. Reference and candidate share the raw corpus, its 256-byte remap, the
exact-corpus XSEP map and the banked native finish. This establishes the
product interface on one 6,311,445-byte synthetic input; not throughput,
physical I/O savings, width qualification or a larger-rung authorization.

## Direct row pipe: the rolling producer now feeds the Rust m20 builder

On the same generated raw corpus and exact XSEP map, `proc_b7c2` ran **front64 pos → parse64 rank → finite_emit64 staged-epochs → xsa ctx-tap → independent raw checker**. The front and ranker generated the inverse afresh; the archived product was a comparison target, never a source of rows or trie bytes. All **6,311,446** forwarded suffix positions, adjacent LCPs and predecessors passed, including the independent final rank. Through three posting-file epochs, the Rust tap built the exact **34,608-byte member 20**, SHA-256 `926ccda7666fbf16bf424b7541bc616e3fb260e6c53de66fecf35dab1add0261`. No position band was written or read (the directory relay used 50,491,568 B each way). This compares actual bytes with a target consumer, not just wire geometry. The new run did **not** rebuild the other nine members or do the final joins. No timing or physical IO comparison. [Result and executable pins](gates/receipts/v10-rowpipe-m20/).

New opt-in Rust CLI (Linux; existing `--bands` / `--sa` modes unchanged):

```text
... row43 producer ... | xsa ctx-tap --row43-pipe STOP --deadline UNIX_SECONDS \
    --corpus RAW --n ROWS --doc-starts EXACT_XSEP_MAP --out NEW_M20 \
    --depth 8 --min-support 1000 --docs-min-support 1000 --threads 1 | ... row43 consumer ...
```

Both ends must be bounded pipes. One 32,768-row packet supplies positions to the **existing** prefix/document lookup and trie encoder and passes the **unchanged** 12-byte rows down the line. Slow consumers backpressure the producer; a separate undrained-pipe test stopped with exit 75 and no m20. The driver must authenticate the exact corpus and map (equal length alone is not identity), run a raw-row witness, and require every exit before using the side payload in a join. Neither passing rows nor publishing m20 publishes an index. This transport removes the separate position files, not the corpus lookups, pipe traffic, builder memory or final payload. It is not wired into default builds.

Fixture: `bash gates/overlap_v10_pipe_fixture.sh` on the rehearsal host with its pinned tools. Shared fixture service: two CPU equivalents, 256 MiB, zero swap; compile separate. No large corpus input or dispatch. The largest finite tie here is just two descriptors, and no coordinate crosses u32: use this for **integration**, not scale claims.

## Reproduce the earlier full v10 reference and band-relay fixture

On the rehearsal host (offline Cargo registry, Linux user systemd, banked
bundle cache required), from an isolated checkout with both git objects:

```sh
D=$(mktemp -d)
gates/overlap_v10_fixture_tools.sh "$D/tools" >"$D/tools.log" 2>&1
python3 -B gates/overlap_v10_tap_test.py
systemd-run --user --wait --pipe --collect --quiet -p MemoryMax=3G \
  -p MemorySwapMax=0 -p CPUQuota=200% -p RuntimeMaxSec=300 \
  python3 "$PWD/gates/overlap_v10_fixture.py" "$D/run" "$D/tools/target/release" \
  >"$D/run.log" 2>&1
```

The tool builder archives the fixed objects into a **new** directory. The
historical Rust driver/tap/member code is compiled with one additional
fixture-only CLI branch. The native bundle is rehydrated from the manifest in
`~/.cache/xsa/13524d6bbedad508fdd8e30e7ab709dbf62ae5db64eb0cd8864dd79efa5b7b4c`;
all listed files are hashed before embedding and the driver verifies the
bundle. No recompile/substitution of the banked native finish. The reference
executes the historical `build --tape` branch, not the new join command.
`RESULTS_REHEARSAL_100GB.md` and the original rehearsal's `xsa-build.log`
pin `6b87aa1620505cff02bc93e6c008483d0d895a83`, with χ on, m14 off, in-build
m20 (8/1000, doc support 1000), and embedded m15. A current build with similar
switch names or a v8 m14 fixture is not the reference.

Tool digests from this build (Python 3.12.3, rustc 1.97.1; newly compiled
shim hashes are receipts, not portable compiler identities):

```
4f73b65c22b4e750e8a7501e24a46c8e3e5d0d43321da5256e9dacf3ca33f753  banked scatter_sort
7571daf5eba713e7bad3e37c1eb33ed80b6e8650c7596ed99b7e1bdd9973539b  banked cross_lcp_merge
bb929a7092cfeba7faa7c37ca999b71563e3af782d73fe693d1a35f5ecdbaaa9  banked xsa_finish
cb9b2dc67b5b951bccc2ec7d78b85e05c7660be71e0d292dada83a17353fbb4b  fixture xsa
c8d723848a1b79c4c8db3d11b2ada0c9b0cab921b5bddd9ae4253d3cea855ab6  fixture_rows
da2a2ad73fa84d77cdbbc97c3abc9f61daa7825dc2fa8f17946aeb065cac195c  pfp_sort_r
```

## Interfaces tested

* Reference CLI: `build --tape --input corpus --scratch ref --threads 1
  --memory-gb 3 --scratch-budget-gb 1 --trie-depth 8 --trie-min-support 1000
  --docs-map map`. Input SHA256 `0bcf20cfcdb2ddc4bd49c52c25d6b4c810c0364fb924c8b54d71968c6cb07539`;
  `seps.bin` SHA256 `7ad88538c8c85a80eca58591a187a27539dea9d988a80c86e7c544372befc8c3`.
  Starts at 0, 3 and 19972 belong to this exact corpus, not inferred from its
  internal `0x1e`. Don't rely on merely opening a map to establish provenance.
* Candidate: `5a1bb56`'s binned `overlap::construct` on mmap text; a callback
  emits little-endian u32 `(position, byte LCP, raw BWT)` triples including
  virtual END first and raw BWT 256 at position zero. One blocked pipe feeds
  the relay. It passes the unchanged triples to the unmodified external sink
  and publishes **one** ordered 32,768-row position band at a time in a private
  directory. This is not `ctx-tap --sa` (which mmaps an entire u40 SA).
  `ctx.rs::tap` expects LE u64 words with position in low 48 bits, ignores
  bits 48..55, rejects nonzero top byte. File close precedes `.opened`; only
  the tap unlinks `.sa` and writes `.consumed`; the relay reaps both markers
  before reusing its credit. Final `end` band count then `end.ready`, only at
  exact triple EOF. 193 bands, 6,311,446 rows; 50,491,568 logical bytes
  written **and** read, ≤262,144 live position-file bytes. This is additional
  traffic versus consuming positions directly, not a claimed disk saving.
  The historical tap also reads mapped corpus prefixes in rank order. The
  unit tests exercise partial last band, malformed/truncated/extra rows and
  BWT END rejection; wire validation alone is not proof of suffix order.
* `pfp_sort_r` external triples path expressly **refuses `TRIE_TAP`**. Do not
  set it: independently teed private position bands are distinct from the
  sink's `band_i.lcp`/piece stream. The existing finish, sink, constructor and
  historical `ctx-tap --doc-starts map` run concurrently (no full SA or
  stored triples). After all four and the relay exit successfully,
  `fixture-join` calls historical `ctx::append_m20` on the finished v7 file,
  releases the external m20, then `docstarts::embed` with `Source::DocsMap`
  and `SAMPLE_STRIDE`. There is no frozen-finish CLI that does both joins.
* Both outputs: version 10, members **1,4,5,7,10,11,12,13,15,20**, χ=5,236,540;
  m20 boundary-aware, 501 nodes/8,789 edges, 34,608 B; m15 3 docs/72 B.
  Whole-container SHA256 `55eea26f9d735664204fb58f33f7c601477bfbecfb8f34d298e6425ce92bcb07`.
  `ctx-verify` brute histograms for `abr` (a document end) and `abra` (a cut
  occurrence) both exact above support; `docstarts verify` 128 rank/select
  samples, zero disagreements. Default corpus-verified `search LEFTRIGHT`
  returns 19980, excludes 19968; `--cross-doc` returns both, flags 19968.

## Lifetime receipt and next integration contract

Both arms and verification ran serially in one dedicated service with a hard
3 GiB RAM ceiling, no swap, two CPU equivalents. Within each arm the live
phases overlap. One thread per native phase; reference witness budget 1 GiB,
fixture candidate 128 MiB. Allocated scratch sampled roughly every 40 ms with
512 MiB alarm, including input/map, both containers, remaining ref artifacts,
logs, tap bands, sink pieces and finish temporaries. This is **not** a hard
filesystem quota or physical I/O count. Whole-service kernel memory peak
318,652,416 B. Phase samples (bytes, including charged cache, not additive):

| interval | whole-fixture allocated disk | service memory.current |
|---|---:|---:|
| reference | 66,236,416 | 317,874,176 |
| candidate concurrent pipeline | 69,730,304 | 168,890,368 |
| candidate in-place joins | 24,260,608 | 56,029,184 |

Sampling can miss the short joins. Account for the simultaneous bytes, not
just the end sizes: the tap finishes builder columns into a payload, writes
one `.partial` then renames it. At m20 join the payload file and its RAM Vec
coexist with the mapped base container and the newly appended member. At
100 GB the banked sizes alone imply **65,497,413,360 + 2,333,923,624 B** on
disk just before deleting the side payload, apart from map/input, all live
scratch and filesystem overhead. m15 follows, with gap/sample buffers plus
encoded bytes, and historical `embed` even reads `seps.bin` into a Vec for
its length metadata. Charge the encoded member and mapping/page cache while
the container grows by m15 + directory delta; don't add an entire second
container or assume that mmap is free. Banked final length 65,533,578,480 B.

**Next seam:** retain these exact order, END, map and join contracts; expose
checked wide decoded positions from the real producer to a bounded tap (or
feed the same builder in process without writing bands), with cancellation
and backpressure spanning *all* consumers. Reject wrong map/corpus identity
before spawn. This toy's u32 triples and in-memory binned constructor are not
the production row43/wide path. Budget builder state, prefix reads and map,
parse/dictionary/occurrence working set, pieces and LCP windows, finish and
both joins against one simultaneous grant. Gate the wide adapter, stop paths,
all members and boundary queries on small inputs before proposing larger
work. No prepared-column benchmark, physical-IO result, or speed comparison
was attempted here.

## Native relay qualification (small fixture only)

Use the historical `tools/target/release` from the build above. The optional
native archive adds **no production gate switch**. For the tagged run the
`5a1bb56` binned constructor emits row43 with the pinned `b9f60c1` encoder.
The real Rust row43 decoder, PieceCutter and slice writer from `b9f60c1` run
in a **fixture-only sink binary**, asserted external input and exactly
6,311,445 corpus bytes. Native narrow mode uses the original external sink.
Both drive the same historical `ctx-tap`, frozen finish and v10 joins.

```sh
gates/overlap_v10_native_tools.sh "$D/native"
python3 -B gates/overlap_v10_relay_test.py "$D/native/relay"
for W in narrow row43; do
  systemd-run --user --wait --pipe --collect --quiet -p MemoryMax=3G \
    -p MemorySwapMax=0 -p CPUQuota=200% -p RuntimeMaxSec=300 \
    python3 "$PWD/gates/overlap_v10_fixture.py" "$D/run-$W" \
    "$D/tools/target/release" --native-tools "$D/native" --wire "$W"
done
```

`relay narrow|row43 CORPUS EXACT_N NEW_BANDS STOP` checks stat extent,
retains 32,768 untouched 12-byte rows and 32,768 decoded LE u64 positions.
It validates adjacent LCP bounds, first virtual suffix, unique raw BWT END,
domain/tag and exact EOF (also exact size for file stdin). It does **not**
certify a permutation or suffix order by itself. It publishes closed position
files to a newly created private directory. Credit is exactly one: neither
next block nor final `end.ready` before both delivery and tap acknowledgement.
For this input the extra logical position traffic is still 50,491,568 B per
direction, not an I/O saving. No SA spool. High-bit tests send one synthetic
block against a sparse metadata-only corpus extent, not a large corpus build.

A supervising fixture is required. To stop, touch `$D/run-$W/stop.requested`.
Nonblocking pipe operations poll for STOP (50 ms), credit waits check every
2 ms. Any nonzero child exit, scratch alarm or deadline stops the pipeline,
signalling protocol interrupt markers and preserving logs, outstanding bands
and partial output. Peers unable to observe flags get TERM/KILL after grace;
no supervisor deletes a band's data or manufactures credit. Only the verified
candidate is renamed `cand.sxi2`. `gates/receipts/v10-relay/stalled.json`
records a real suspended ctx-tap (`SIGSTOP`, `--stream-timeout 5`): relay
observed STOP, intact band 0, no consumed/end marker or published candidate.

`gates/receipts/v10-relay/row43.json` records the tagged whole-file equality,
member IDs, boundary checks, tool digests and cgroup samples. Candidate stream
wait 6.15 s plus 0.04 s join vs 10.21 s reference wait, from raw small input
with reference remap reused; not a matched speed or physical I/O benchmark.
No prepared-columns measurement. No scale authorization: >u32 transport does
not qualify real wide parse/dictionary/rank construction, tap builder RAM and
rank-order prefix locality, position-band traffic, simultaneous sink pieces /
LCP / witness and page-cache lifetimes, or the v10 join peaks detailed above.
