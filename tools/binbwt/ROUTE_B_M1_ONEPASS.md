# Route B: one-pass m1 (the rlebwt wire is never written)

Status: implementation gated at the 454,752-byte fixture, 150 MB and 10 GB.
The legacy two-pass arm remains available (`CROSS_RLEBWT=1` + `xsa_finish
--m1-replay`) and is byte-identical to the banked container SHAs.

## 1. What the two passes were for

`m1` is the container's run member: `{C table 256x u64, 256 code lengths,
head bit count, length bit count, Huffman symbol bits, gamma length bits}`.
The head stream is a Huffman code over the **normalized run symbols**; the
length stream is gamma-coded and stateless.

The head codebook was built from `runFreq[]`, the per-symbol count of emitted
runs. That vector is only complete when the sweep ends. So the producer staged
every run in the `frag.rlebwt` wire (4+ bytes/run) and `m1` replayed it from the
beginning to encode. At 10 GB the wire measured **20,917,776,384 allocated
bytes (2.0918n)** against a whole-run peak of 3.9756n: more than half the peak,
held for the entire sweep, in exchange for a codebook the finish had almost
when it needed it.

## 2. Why one pass is enough

The BWT is a **permutation** of the text. The histogram of the symbols it emits
is therefore the histogram of the text's bytes - the remapped corpus byte
histogram - which is knowable *before the first run exists*, and the finish
already owns the corpus. Only the head stream needs a codebook, so `m1` can
encode as the sweep produces runs:

* symbol -> canonical Huffman code from the corpus byte histogram,
* length -> the existing gamma code (needs no codebook at all).

What the corpus histogram is **not** is the histogram of run *starts*: a byte
that forms long runs starts fewer runs than its byte share, so the corpus code
is slightly longer than the exact-run code. That costs container bytes, not
correctness: `m1` publishes its 256 code lengths, so the reader is unchanged and
the stream decodes exactly. Measured on the accepted 10 GB container
(`tools/binbwt/m1_stream`, which reports both codes from one published m1):

| n | exact-run code bits/run | corpus-histogram code bits/run | growth |
|---|---:|---:|---:|
| 454,752 | 5.131036 | 5.197114 | +1.29% |
| 150,000,001 | 5.296106 | 5.382637 | +1.63% |
| 10,000,017,258 | 5.311431 | 5.411692 | +1.89% |

At 10 GB that is +0.100 bits/run = +43.5 MB = **+0.0044n** of container bytes,
against -2.09n of scratch. Run entropy 5.276329, corpus byte entropy 4.987889,
cross-entropy 5.426840: the codebook is a proxy, and the proxy is priced.

## 3. The canonical derivation (reproducible)

`canonical_huffman_m1` in `bit6/xsa_finish.cpp`:

1. `w[c] = max(corpus byte count of c, 1)`. The +1 pseudo-count is what makes
   the code **total**: all 256 symbols get a code, no length is ever zero, and a
   symbol the run stream emits but the corpus histogram (for that alphabet)
   happens to under-weight can still be encoded.
2. Huffman tree, priority queue keyed on `(weight, creation index)` - the same
   tie-break the frozen legacy derivation uses.
3. Lengths from an iterative depth-first walk; a leaf's depth is its length.
   Depths are asserted to be in `[1, 63]`.
4. Canonical codes: sort symbols by `(length, symbol)` ascending, then
   `code = 0; for each: code <<= (len - prev_len); codes[sym] = code++;
   prev_len = len`.

That is exactly the form the reader reconstructs from the 256 published lengths
alone (`tools/binbwt/stream_vs_bwt.cpp` `M1`, and `xsa/src/sxi2.rs`), so nothing
new is published and the container format is untouched.

## 4. What changed

* `bit6/binbwt_slice.h` (`BinBandSink`, the fused tape producer): does not
  create, extend or publish the wire. No band extents, no `rlebwt.meta`, and the
  per-band rlebwt word vector (13.9e9 words at 10 GB) is not even built.
  `CROSS_RLEBWT=1` restores all of it, byte-identically.
* `bit6/cross_lcp_merge.cpp` (`SequentialV2` and `emit_sparse_v2`): the same
  gate for the non-tape v2 producer; its shape/report line
  `CROSS_V2_WIRE rlebwt=0 ...` states the arm. The non-v2 four-file emitter
  (`emit_four_ext`) is untouched - its `.ssa/.ssa_t/.rlebwt` set is a banked
  contract with other consumers - and the production-oracle selftest
  **force-enables** the wire, so removing it from the product route does not
  remove the comparison that validates it.
* `bit6/xsa_finish.cpp`:
  * `corpus_byte_histogram()`: the remapped corpus histogram over the adapter's
    own byte path, 8 threads, strided 1 MiB blocks, started as a helper thread
    while the producer publishes its wire and **joined before the first run is
    encoded**. It cannot change what the sweep emits - only how the first bit is
    written.
  * `canonical_huffman_m1()`: section 3.
  * `bandSink` encodes each emitted run in stream order: `msb(code, len)` for
    the symbol, `gamma(len)` for the length. No staging, no replay.
  * `--m1-replay` (or `XSA_FINISH_M1_REPLAY=1`) restores the frozen two-pass
    block verbatim, including the rlebwt wait, the replay, and the
    `rs2.totals/runFreq` divergence guard.

### Guards added (none weakened)

* 256 exact equalities between the corpus byte histogram, the codebook weights
  and the emitted symbol totals (`XSA_FINISH_M1_HISTOGRAM ... agreement=exact`).
  This is the "the BWT is a permutation of the text" premise, checked per cell
  instead of assumed.
* the encoded run count must equal `rNorm` (`m1 encoded run count`).
* route B **fails loud** if an `rlebwt` wire exists: a stale wire means the
  producer was not told `CROSS_RLEBWT=0` and the scratch claim would be false.
* `--keep-rlebwt` with route B is refused (it asks for a file that arm never
  creates).

## 5. Gates

See `evidence/route-b-m1/` for the raw results and component series.

| rung | arm | chi | runs | closure | full stream check | container sha256 |
|---|---|---:|---:|---|---|---|
| 454,752 B | route B | 145685 | 192138 | 0/0/0, fallbacks 0 | rows=454752 runs=192138 mismatches=0 | `cdb1a1e89bed6c23dec6eede48c5c6765c41cc08422355f585c6f3c93f1a0a1d` |
| 454,752 B | legacy replay | 145685 | 192138 | 0/0/0, fallbacks 0 | rows=454752 runs=192138 mismatches=0 | `98f1f6dc82befd1659112678bbc3debcf6c40ec417fcea86df47a8a51019c658` (banked) |
| 150 MB | route B | 44034231 | 57279187 | 0/0/0, fallbacks 0 | full BWT comparison, 0 mismatches | `5778d943ffc90dfef173b1a02f4224f15f7598c09e5877e082178421e74a19d5` |
| 150 MB | legacy replay | 44034231 | 57279187 | 0/0/0, fallbacks 0 | full BWT comparison, 0 mismatches | `d7eeac0f574a9a6e272732737d20b6d18a533336fea8f46f10287cb0e2b063a5` (banked, reproduced) |
| 10 GB | route B | 2676929428 | 3474891366 | 0/0/0, fallbacks 0 | rows=10000017258 runs=3474891366 mismatches=0 (FULL, vs the 80 GB SA) | `15e0634fa27582c587827e5ad9af05126168aea5fc13957ee7bc063d93659001` |
| 10 GB | legacy replay | 2676929428 | 3474891366 | 0/0/0, fallbacks 0 | rows=10000017258 runs=3474891366 mismatches=0 (FULL) | `22d0ce7493c6238a9d2e7f4e9a0decdf5841ff72a845d856bebaecd6d34bdaee` (banked, reproduced) |

The 10 GB corpus is `pile-10b-snap.txt` (n = 10,000,017,258), the read-only
full-SA fixture is `r10sf/sa` (80 GB), K=16, `--wave-cap-gb 10.0`, threads 96.
Both arms: six finish selftests, checker selftest, sampled 4096-row comparison
and the FULL 10,000,017,258-row BWT comparison against the SA fixture pass with
zero mismatches.

`chi` and `runs` do not move in either arm. The container SHA changes for route
B by construction (a different - slightly longer - codebook); it is recorded
here as the post-Route-B baseline. The legacy arm still reproduces the banked
SHAs exactly, which is what makes it a usable A/B reference.

### The round-trip gate

`tools/binbwt/m1_stream CONTAINER --compare OTHER` decodes the `m1` member of
two containers and compares the decoded normalized run stream **run by run** -
`(char, length)` in order, exact, no sampling:

* 454,752 B: `M1_AB runs=192138 mismatches=0 equal=1`, and the materialized
  5-byte-per-run record dumps are `cmp`-identical (960,690 bytes).
* 150 MB: `M1_AB runs=57279187 mismatches=0 equal=1`, dumps `cmp`-identical
  (286,395,935 bytes, sha256 `29672e4d78270ffb5aabc6ebb48552ba49e40ef065bd9444abfc5a56d5db173e`).
* 10 GB: `M1_AB runs=3474891366 mismatches=0 equal=1`. Both containers' decoded
  streams carry the same fingerprint `367d0b815bcc8029`, which is also the
  fingerprint measured on the pre-Route-B banked container before any of this
  work started.

Round-trip at the pile is therefore established three ways: the A/B decode
(3.47e9 runs, exact), the fingerprint chain back to the banked container, and
the full independent `STREAM_VS_BWT` comparison of every row against the SA.

## 6. The container grows by exactly the predicted amount

10 GB, `m1_stream` on both containers:

* legacy: `head_bits=18456646620` = 5.311431 bits/run, container 4,463,793,538 B
  (0.446378n);
* route B: `head_bits=18805043158` = 5.411692 bits/run, container
  4,507,343,105 B (0.450734n).

+348,396,538 bits = **+43.5 MB = +0.004356n**, against a 2.09n wire. The
`5.411692` the route-B container encodes at is *exactly* the value
`m1_stream` predicted from the legacy container's C table before the run -
the codebook is where the arithmetic said it would be.

## 7. Scratch at 10 GB

Route B, component sampler at a 0.25 s target period (median achieved period
0.2515 s, p90 0.381 s; the `du` traversal itself is 49.7% of one core over the
run and 0.21 s mean per sample, so it is an observed lower bound, not a
continuous maximum). Whole-run peak **21,769,482,240 bytes = 2.176944n** at
t=2064.4 s (end of the sweep), against the banked legacy peak 3.975649n (1 s
sampling) / 3.672099n (1 s sampling, pinned corpus).

| Component | Bytes at whole peak | n at whole peak | Independent peak bytes | Independent peak / n |
|---|---:|---:|---:|---:|
| waves | 11,110,723,584 | 1.111070 | 13,777,731,584 | 1.377771 |
| pfck2 | 3,636,379,648 | 0.363637 | 3,636,465,664 | 0.363646 |
| witness_spills | 2,710,347,776 | 0.271034 | 3,035,738,112 | 0.303573 |
| bits-head | 2,135,949,312 | 0.213595 | 4,284,481,536 | 0.428447 |
| pieces | 1,141,280,768 | 0.114128 | 1,167,179,776 | 0.116718 |
| bits-len | 989,855,744 | 0.098985 | 1,075,838,976 | 0.107584 |
| sparse2 | 33,816,576 | 0.003382 | 33,816,576 | 0.003382 |
| other (logs, sampler, sample) | ~7.9e6 | 0.0008 | | |
| **TOTAL** | **21,769,482,240** | **2.176944** | | |

n-sized components that are NOT alive at that instant, with their own peaks:
`container` 8,591,900,672 (0.859189 allocated; its file is 4,507,343,105 B =
0.450734n), `packed_bands` 3,951,792,128 (0.395179), `bits-high`
2,016,411,648 (0.201641), `bits-low` 403,701,760 (0.040370).

**Is the peak <= 2n? No.** 2.176944n observed. The arithmetic:

* the wire is gone: `rlebwt` is never written, sampled at 0 bytes across 12,711
  samples, and the producer reports `CROSS_RLEBWT_ABSENT`;
* but the m1 head+length payload (0.29n logical, 0.32n allocated at the peak
  instant, up to 0.536n allocated at its own peak) now exists DURING the sweep
  instead of after it, so it overlaps the producer's wave scratch. That shift
  gives back ~0.27n of the 2.09n the wire cost;
* the residual is dominated by terms that are not m1's: producer waves
  1.111-1.378n, pfck 0.364n, witness spills 0.271-0.304n, pieces 0.114n.

A pessimistic coincident-the-peaks bound over the sweep-phase components
(waves + pfck + witness + pieces + bits-head + bits-len + sparse) is 2.701n;
the observed 2.1769n is what a 0.25 s sampler actually saw.

XFS speculative allocation is included, not subtracted - and it is large on the
two files this arm writes: `bits-head` allocates 4,284,481,536 B for a
2,350,630,395 B stream (1.82x) and the container allocates 8,591,900,672 B for
4,507,343,105 B (1.91x). Clamping those with `posix_fallocate` (reserve exactly
what is written) is the next named lever, as is the producer's wave cap
(`--wave-cap-gb 10.0` = 1.00n logical, 1.38n allocated).

### The A/B that matters: same binaries, same args, same 0.25 s sampler

Both 10 GB arms ran the identical bundle, sorter and driver on the same corpus
with K=16 and `--wave-cap-gb 10.0`, sequentially:

| arm | observed whole peak | n | rlebwt at the peak |
|---|---:|---:|---:|
| legacy replay (`CROSS_RLEBWT=1` + `--m1-replay`) | 35,169,693,696 B | 3.516963 | 1.642880 |
| route B (default) | 21,769,482,240 B | 2.176944 | 0 |

**-1.340019n (-38.1%)**. The legacy peak instant is
`rlebwt 1.642880 + waves 1.120880 + pfck2 0.363637 + witness_spills 0.271087 +
pieces 0.113591 + sparse2 0.004089`; the route-B peak instant is the same list
with `rlebwt` gone and `bits-head 0.213595 + bits-len 0.098985` present, because
m1's payload now exists while the sweep runs. A coincident-the-peaks bound over
the sweep-phase components is 4.319n for the legacy arm (2.676n + the wire's
1.643n) and 2.701n for route B.

The banked 1 s-sampled baselines are 3.975649n and 3.672099n; this run's legacy
arm measured 3.516963n at 0.25 s. Those three numbers are the same pipeline
measured differently: the sampler's landing point decides how much allocated
state it witnesses, which is why the A/B above (one sampler, one code) is the
only comparison quoted as a saving.

### Walls (single runs, same 0.25 s sampler; no speed claim)

| rung / arm | order | emit | finish | full check |
|---|---:|---:|---:|---:|
| 10 GB route B | 2171.31 | 2171.31 | 2282.27 | 1322.39 |
| 10 GB legacy replay | 1933.83 | 1933.83 | 2186.85 | 850.66 |

The `order` stage is the SAME binary with the SAME input in both runs and its
wall moved 12.3% (1933.83 vs 2171.31); that is this box's run-to-run spread, and
it is larger than the emit delta (+12.3%) and the finish delta (+4.4%). So the
wall numbers are recorded, not interpreted: no speedup is claimed, and no wall
is attributed to route B.

The producer's own accounting (`TAPE_EMIT_DONE`): route B
`wall=2155.93 read_s=35.90 word_s=24.93 publish_s=949.72`; legacy
`wall=1992.84 read_s=36.56 word_s=25.28 publish_s=801.42` (banked, 1 s
sampler). `word_s` is the rlebwt word stream that route B does not build; it is
small because that stream was buffered per band, but the publish path also loses
the 13.9e9-word wire writes. These walls are NOT comparable across sampler
periods: the 0.25 s sampler is a heavier load on the same filesystem than 1 s
(see above), and no paired N>=3 comparison was run. No speedup is claimed.
