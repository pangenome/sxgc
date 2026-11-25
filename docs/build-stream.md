# Build a v10 index from a raw corpus

```sh
cargo build --release --locked --manifest-path xsa/Cargo.toml -p xsa
xsa/target/release/xsa build-stream corpus.bin --docs-map documents -T /scratch -o corpus.sxi2
xsa/target/release/xsa query corpus.sxi2 --count 'search term'
```

`corpus.bin` is a nonempty regular file. Its writer must be finished. The builder reads it in place. `documents/seps.bin` is an XSEP v1 document map for those bytes; the map may cover a longer stream if the input is its prefix starting at byte zero. The last document ends at the input length. Document boundaries are never guessed from delimiter bytes. As in the existing v10 format, one of the 256 byte values must be unused so the alphabet has a code for END.

`-T DIR` selects an ordinary scratch directory (default: `$TMPDIR`, otherwise the output's parent). `-j N` selects sort workers; the context and container workers also run during emission. `-m SIZE` sets the sort-array budget (binary `K`, `M`, `G`, `T` suffixes), **not** a process RSS limit. It includes the raw mapping and sort arenas but not the other workers, index directories, trie or kernel page cache. The default is `max(256 MiB, 2 × input bytes)`. Use `-v` for diagnostics and phase times. Normally success is silent. Errors go to stderr and return nonzero. An existing output is not overwritten.

## Data path

1. The front reads the corpus, counts its alphabet, orders the overlapping phrases, and writes their lexical keys in text order. It **does not materialize a phrase dictionary**. Rank the key column and discard it.
2. Read the inverse phrase ranks and corpus. Count finite-key ranges and encode their monotone postings in batches reusing one ordinary scratch file. Each range is sorted and expanded, keeping complete ties and their continuation ranks together. The output is one ordered pipe of `(position, adjacent LCP, predecessor)` rows, including the virtual END. No on-disk byte suffix array or LCP column.
3. The context worker uses those positions and the document map to build m20. It forwards the packets to the container writer. After sorter EOF it sends the encoded m20 and m15 down the same pipe. Pipe backpressure bounds how far stages can run ahead of each other.
4. The writer carries **one open BWT run** across packets. Its first LCP and minimum interior LCP feed the χ sweep; its head, tail and length feed the run samples and m1 encoder. It writes codec blocks into the output as they close. The χ bitmap is one bit per row; its low and high Elias–Fano streams are written directly into m5. Append the directories, m20 and m15 and close the v10 container. There are no piece/slice handoff files, PFCK fingerprint file, checkpoints or verifier processes. Member CRCs belong to the file format. There is **no output read-back** or reference-index input.

The output is staged in its own parent directory and published with an atomic no-replace link only after all workers succeed. Ordinary exit, error, Ctrl-C and SIGTERM clean up private temporary directories. SIGKILL or power loss may leave `.xsa-stream-*` / `.xsa-publish-*` directories; this is a fresh-build tool, not a resumable job manager.

Scratch planning covers the larger of the key/inverse overlap and the concurrent inverse, posting file, histogram and χ bitmap; there is a 64 KiB floor for small files. Filesystem metadata, allocation rounding and speculative allocation are filesystem-dependent. There is no mount, image, quota or promise that a logical byte count is a physical one. If a parse or a complete posting batch will not fit, the sorter fails rather than dropping a tie or publishing a partial index. A disk-full error is fatal.

For a repeat build with a trusted **whole-product** checksum, run `sha256sum -c expected.sha256` after success. The compact CLI regression is `python3 xsa/tests/build_stream.py xsa/target/release/xsa`; it is not called during a build.
