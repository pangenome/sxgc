# Small exact-v10 fixture (qualification, not a scale gate)

**Result:** all 8,847,248 bytes match, including the directory and provenance.
The candidate consumes overlap-produced rows, not reference SA/BWT/LCP or trie
bytes. Reference and candidate share the raw corpus, its 256-byte remap, the
exact-corpus XSEP map and the banked native finish. This establishes the
product interface on one 6,311,445-byte synthetic input; not throughput,
physical I/O savings, width qualification or a larger-rung authorization.

## Reproduce

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
