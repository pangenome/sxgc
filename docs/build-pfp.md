# `xsa build --pfp`: build a v10 index from a raw corpus

Select PFP with `--pfp` instead of `--tape` or `--bin`. This replaces the standalone `xsa build-pfp` (formerly `build-stream`); the existing PFP arguments, defaults and v10 format are unchanged. Omitting `--pfp` does not select this path.

The whole build is orchestrated by `xsa` itself (`xsa/src/build_pfp.rs`): it parses the arguments, plans scratch, drives the rank/lift census recursion, runs the stages through bounded pipes, and publishes the finished container with an atomic no-replace link. The stages are the bundled binaries (`pfp_parse`, `stream_rank`, `pfp_lift`, `pfp_rows`, `xsa_finish`) plus the `xsa stream-context` stage of the same executable. No interpreter is started during a build and the bundle carries no build script, so the only entry point on this path is `xsa build --pfp`.

## Input adapters

`xsa build` reads raw bytes: this route and the bin/tape chain (`--input` /
`--tape` / `--bin`) both consume a finished byte stream. The genomic source
formats (`--agc`, `--fasta`, `--fastq`) have **no route** in this build: the old
Python source pipeline was removed with the shell phase wrapper, and
`xsa build --agc|--fasta|--fastq` refuses before opening anything and writes
nothing. Those adapters (AGC / FASTA / FASTQ -> raw tape) are being ported to
Rust; when they land they emit the same raw input the chain already takes, so
every downstream phase stays as it is.

```sh
cargo build --release --locked --manifest-path xsa/Cargo.toml -p xsa
xsa/target/release/xsa build --pfp corpus.bin --docs-map documents -T /scratch -o corpus.sxi2
xsa/target/release/xsa query corpus.sxi2 --count 'search term'
```

Pass the corpus as `corpus.bin` or `--input corpus.bin`, not both. It must be a nonempty regular file. Its writer must be finished. The builder reads it in place. `documents/seps.bin` is an XSEP v1 document map for those bytes; the map may cover a longer stream if the input is its prefix starting at byte zero. The last document ends at the input length. Document boundaries are never guessed from delimiter bytes. As in the existing v10 format, one of the 256 byte values must be unused so the alphabet has a code for END.

`-T DIR` (or `--scratch DIR`) selects an ordinary scratch directory (default: `$TMPDIR`, otherwise the output's parent). `-j N` selects sort workers; the context and container workers also run during emission. `-m SIZE` sets the sort-array budget (binary `K`, `M`, `G`, `T` suffixes), **not** a process RSS limit. It includes the raw mapping and sort arenas but not the other workers, index directories, trie or kernel page cache. The default is `max(256 MiB, 2 × input bytes)`. Use `-v` for diagnostics and phase times. Normally success is silent. Errors go to stderr and return nonzero. An existing output is not overwritten.

## Data path

1. `pfp_parse` makes one trigger scan. It names **overlapping** phrases by their finite contents and writes [dense lexical IDs in text order, dictionary references and the exact census](pfp-handoff.md). It does not copy the dictionary text. The rank stage takes this tape `P1` and returns one-based suffix ranks in original position order (END rank 0). When the [integer census](pfp-handoff.md#recursive-id-levels) warrants it, the builder parses whole IDs, sorts only the deepest tape with `stream_rank inverse`, then uses `pfp_lift` to induce ranks up through the levels. No level reads the raw text to sort ID suffixes. With `-v`, `PFP_CENSUS` reports the decision and `PFP_LEAF` identifies the tape actually sorted.
2. `pfp_rows` obtains byte starts by summing the tape's owned strides. Frequencies give each phrase's exact contiguous interval in parse rank order; its occurrences already follow successor rank. There is no second trigger scan. A count pass over **owned offsets of distinct phrases** determines disjoint two-symbol ranges; one distribution pass writes compressed postings for all ranges. Read each posting once and order the finite representatives inside its range. For equal representatives merge/select by the already known next parse ranks. Carry the incoming byte LCP at each group/range boundary. Output is one ordered pipe of `(position, adjacent LCP, predecessor)` with the virtual END; no on-disk byte suffix array, LCP column or global row merge.
3. The context worker uses those positions and the document map to build m20. It forwards the packets to the container writer. After sorter EOF it sends the encoded m20 and m15 down the same pipe. Pipe backpressure bounds how far stages can run ahead of each other.
4. The writer carries **one open BWT run** across packets. Its first LCP and minimum interior LCP feed the χ sweep; its head, tail and length feed the run samples and m1 encoder. It writes codec blocks into the output as they close. The χ bitmap is one bit per row; its low and high Elias–Fano streams are written directly into m5. Append the directories, m20 and m15 and close the v10 container. There are no piece/slice handoff files, PFCK fingerprint file, checkpoints or verifier processes. Member CRCs belong to the file format. There is **no output read-back** or reference-index input.

The output is staged in its own parent directory and published with an atomic no-replace link only after all workers succeed. Ordinary exit, error, Ctrl-C and SIGTERM clean up private temporary directories. SIGKILL or power loss may leave `.xsa-pfp-*` / `.xsa-publish-*` directories; this is a fresh-build tool, not a resumable job manager.

Scratch planning covers live phrase-naming runs, ancestor tapes/refs, child ranks, lift postings, the lower inverse, the histogram and the concurrent χ bitmap. The row producer closes and unlinks its consumed columns **before** allocating postings. There is a 64 KiB logical budget floor for small files. Filesystem metadata, allocation rounding and speculative allocation are filesystem-dependent. There is no mount, image, quota or promise that a logical byte count is a physical one. Disk-full is fatal.

**Current limits.** The deepest tape must fit the integer kernel. Each lift must fit its smaller occurrence tables, largest first-ID cell and paged sort/output workspace, and have scratch for its dictionary-offset postings. A shortened upper tape alone does not guarantee any of those, or a speedup. The row-view budget still includes the raw mapping and packed aligned-rank tables. If one two-symbol cell exceeds the available descriptor slots, or the complete compressed posting index exceeds the scratch grant, the producer refuses. It does **not** fall back to a byte suffix sort or rescan all offsets for each range. Historical rolling runs through 10 GB measured a different producer and do not establish fit or speed for this version.

## Measured scaling

The construction phases scale with workers; the emission chain is the floor.
[Measured ladder and method](../gates/receipts/pfp-scale/): 100 MB in 58.4 s of
which 4.5 s is naming and 6.7 s row ordering; 1 GB naming 14.4 s and row ordering
102 s; 10 GB naming 168 s and rank 3.9 s for 318,164,488 phrases. Every published
product matched its earlier whole-v10 SHA.

Three mechanisms carry that: phrase occurrences are sorted on the first two
eight-byte words of their finite window (the exact comparator only breaks key
ties), the occurrence set is sorted in parallel chunks and merged into a second
buffer, and the input is mapped rather than read through a 4 KiB page cache.
Row ordering uses a 16-byte arena record — the eight bytes past the band's known
prefix, plus the packed dictionary code — which is the 16 bytes per descriptor
the planner already reserved.

What remains single-threaded is the row pipeline's two consumers: the m20 context
builder (about 202 s per GB) and the v10 writer (about 181 s per GB, of which 98 s
is the per-row loop and 56 s the χ sweep). At 10 GB they, not the sorter, set the
wall time. m20 and m15 are container members, not options.

For a repeat build with a trusted **whole-product** checksum, run `sha256sum -c expected.sha256` after success. The compact CLI regression is `python3 xsa/tests/build_pfp.py xsa/target/release/xsa`; it is not called during a build. [Qualification and clocks](../gates/receipts/pfp-levels/) distinguish a three-level build, a real prepared-tape lift at 96 MiB, and the ordinary 100 MB real v10 build at 320 MiB.
