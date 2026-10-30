# ACCEPTANCE — `xsa corpus` (Wikipedia dump -> corpus + document sidecars)

Tool: `xsa corpus --wikipedia <dump.xml.bz2|-> --out <dir> [--max-bytes 90G] [--separator 0x1e|none]`
Source: `xsa/src/corpus.rs` (new module) + dispatch in `xsa/src/main.rs`; the sidecar
layouts are documented in the module header and echoed into every MANIFEST.json.
Build: `cargo build --release --manifest-path xsa/Cargo.toml`.
binary: xsa sha256=cc352a52e5189f8e80b02f6c4a8e08c343daffc38035a21d79594b0f0f36d0d8

Fixture (committed): `xsa/tests/wikicorpus/fixture.xml` — 2304 bytes of hand-written
MediaWiki-shaped XML with 5 pages: a real `<siteinfo>` block plus namespaces table, an
escaped-entity body, a `Category:` (ns 14) title that is indexed like any other (no
namespace filtering), an empty `<text></text>`, a `<text deleted="deleted" />` page,
and a multi-line body with a literal tab, trailing spaces and `&amp;`. The tool is fed
**multistream-shaped** bz2 files (3 concatenated `bzip2 -c` streams), the shape real
`pages-articles-multistream` dumps have. `xsa/tests/wikicorpus/fixture_0x1e.xml` is the
same file with one raw 0x1E byte inside page 3's `<text>` (the rule-1 collision).

Every command was run under `timeout 900`; nothing larger than a few MB was processed
and no Wikipedia data was downloaded. Verifiers are inline in the commands below (no
verifier script is committed).

## W1 — 0x1E inside a body is REFUSED (fatal; nothing is published)

```
xsa: error: 0x1E occurs inside document content (count=6 docs=5); rerun with --separator none
xsa: note: 1 of those 6 0x1E bytes came from document content (1 document(s) affected)
xsa: note: nothing was published; the incomplete sidecars keep their .partial names
exit=1
$ ls w1
  total 0
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:32 corpus.bin.partial
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:32 seps.bin.partial
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:32 titles.bin.partial

```

## W2 — clean fixture: byte-exact corpus.bin, seps.bin starts, titles.bin round-trip

```
xsa corpus: docs=5 corpus_bytes=268 separators=5 separator=0x1e seps.bin=76 titles.bin=134 sha256(corpus.bin)=f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w2/MANIFEST.json
exit=0
  pages=5 expected_bytes=268 corpus.bin_bytes=268
  corpus.bin BYTE-IDENTICAL to the (title\n body \x1e) reconstruction
  seps.bin deltas -> every start offset [0, 78, 129, 153, 172] anchors [0] ; each doc ends at its 0x1E
  titles.bin round-trips: ['Anarchism', 'Béla Bartók', 'Category:Living people', 'Deleted text page', 'Tab\tand trailing spaces']
  MANIFEST: doc_count=5 separator_count=5 corpus_bytes=268; 3 sidecar sha256 + compressed-dump sha256 agree
  W2 PASS
```

## W3 — `--separator none`: no 0x1E in corpus.bin, seps.bin still yields every start

```
xsa corpus: docs=5 corpus_bytes=263 separators=0 separator=none seps.bin=76 titles.bin=134 sha256(corpus.bin)=8add71d7a423db26fa32ef7eec3f9379d4dd00d7ad0248197fe291f23fd78c93
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w3/MANIFEST.json
exit=0
$ tr -dc '\036' < w3/corpus.bin | wc -c
0
  corpus.bin carries 0 0x1E bytes and is byte-exact; seps.bin flags=0, doc_count=5, corpus_bytes=263
  seps.bin deltas -> every start offset [0, 77, 127, 150, 168] anchors [0] (seps.bin still 76 bytes: same layout as 0x1e mode)
  W3 PASS
$ xsa corpus --wikipedia fixture_0x1e.ms.bz2 --out w3b --separator none   # the escape hatch W1 names
xsa corpus: docs=5 corpus_bytes=273 separators=1 separator=none seps.bin=76 titles.bin=134 sha256(corpus.bin)=bfba84970a64315bc522307e6d0655dcb2e06ea20989e21e6611c421c4473b93
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w3b/MANIFEST.json
exit=0
  docs=5 content_separator_bytes=1 documents_with_content_separator=1 separator_count=1 (the byte is kept as content)

```

## W4 — `--max-bytes` cuts at a document boundary and reports the exact cut

```
  full-corpus boundaries: 78 129 153 172 268
xsa corpus: --max-bytes 78 reached: cut at document boundary after doc 1: corpus_bytes=78 docs=1
xsa corpus: docs=1 corpus_bytes=78 separators=1 separator=0x1e seps.bin=60 titles.bin=35 sha256(corpus.bin)=ccb81e028e045b88a57b0b5c5fe5d5dd9a57451638c550797a16af9830bca136
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w4-78/MANIFEST.json
  exit=0
xsa corpus: --max-bytes 130 reached: cut at document boundary after doc 3: corpus_bytes=153 docs=3
xsa corpus: docs=3 corpus_bytes=153 separators=3 separator=0x1e seps.bin=68 titles.bin=82 sha256(corpus.bin)=e60b5f3dcbc8815bcda53810f4200e2c9853f4c8ce629645a7db9eccaf3505fe
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w4-130/MANIFEST.json
  exit=0
xsa corpus: --max-bytes 268 reached: cut at document boundary after doc 5: corpus_bytes=268 docs=5
xsa corpus: docs=5 corpus_bytes=268 separators=5 separator=0x1e seps.bin=76 titles.bin=134 sha256(corpus.bin)=f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w4-268/MANIFEST.json
  exit=0
xsa corpus: docs=5 corpus_bytes=268 separators=5 separator=0x1e seps.bin=76 titles.bin=134 sha256(corpus.bin)=f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w4-90G/MANIFEST.json
  exit=0
  max-bytes 78: cut=True docs=1 corpus_bytes=78 cut_corpus_bytes=78 (prefix of full, a boundary member, sidecars agree)
  max-bytes 130: cut=True docs=3 corpus_bytes=153 cut_corpus_bytes=153 (prefix of full, a boundary member, sidecars agree)
  max-bytes 268: cut=True docs=5 corpus_bytes=268 cut_corpus_bytes=268 (whole of full, a boundary member, sidecars agree)
  max-bytes 90G (> fixture): no cut, docs=5 corpus_bytes=268
  W4 PASS

```

## W5 — determinism: same dump -> byte-identical sidecars

```
  w5a exit=0
  w5b exit=0
  w5c --separator none exit=0
  w5d --separator none exit=0
  w5a/corpus.bin f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
  w5a/seps.bin c503f160d7b43a3a574bd91102cc58ad66e30c15360fdda418286d3665f3466d
  w5a/titles.bin f637a27c3cefa6ddfcbe2e8e631ca82f5223456b3e2dd5964d641c8f2fb76c82
  w5b/corpus.bin f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
  w5b/seps.bin c503f160d7b43a3a574bd91102cc58ad66e30c15360fdda418286d3665f3466d
  w5b/titles.bin f637a27c3cefa6ddfcbe2e8e631ca82f5223456b3e2dd5964d641c8f2fb76c82
  w5c/corpus.bin 8add71d7a423db26fa32ef7eec3f9379d4dd00d7ad0248197fe291f23fd78c93
  w5c/seps.bin c7ab485abd331bfffad34d506caccfc5cecdb3e7a82f0a9b186d2be764533908
  w5c/titles.bin f637a27c3cefa6ddfcbe2e8e631ca82f5223456b3e2dd5964d641c8f2fb76c82
  w5d/corpus.bin 8add71d7a423db26fa32ef7eec3f9379d4dd00d7ad0248197fe291f23fd78c93
  w5d/seps.bin c7ab485abd331bfffad34d506caccfc5cecdb3e7a82f0a9b186d2be764533908
  w5d/titles.bin f637a27c3cefa6ddfcbe2e8e631ca82f5223456b3e2dd5964d641c8f2fb76c82
  cmp w5a/corpus.bin w5b/corpus.bin: identical
  cmp w5c/corpus.bin w5d/corpus.bin: identical
  cmp w5a/seps.bin w5b/seps.bin: identical
  cmp w5c/seps.bin w5d/seps.bin: identical
  cmp w5a/titles.bin w5b/titles.bin: identical
  cmp w5c/titles.bin w5d/titles.bin: identical
  MANIFEST.json identical apart from finished_at
$ second pass into a directory that already holds one (now refused before the dump is hashed)
xsa: error: corpus: /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/w5a/corpus.bin already exists; refusing to mix a new pass into an old directory
  exit=1
  W5 PASS
```

## Refusal and robustness gates (rule 5: never emit a short corpus)

### X1 truncated bz2 (multistream cut at 900 of 1185 bytes)

```
  xsa: error: corpus: decompressor failed (exit status: 2): truncated or invalid bzip2 stream; refusing to emit a short corpus
  exit=1

```

### X2 corrupt bz2 (one byte at offset 400 zeroed)

```
  xsa: error: corpus: decompressor failed (exit status: 2): truncated or invalid bzip2 stream; refusing to emit a short corpus
  exit=1

```

### X3 valid bz2, XML truncated mid-page (900 of 2304 bytes)

```
    /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/extra/xmltrunc.bz2: ok
xsa: error: corpus: truncated or malformed XML: stream ended at offset 900 with <revision> still open
exit=1

```

### X4 plain XML handed in as the dump

```
xsa: error: corpus: fixture.xml does not start with 'BZh': not a bzip2 stream (pass the compressed dump)
exit=1

```

### X5 mismatched close tag (</page> -> </pag>)

```
xsa: error: corpus: malformed XML: </pag> at offset 1312 closes <page>
exit=1

```

### X6 <text> as a direct child of <page> (not inside <revision>)

```
xsa: error: corpus: malformed XML: <text> at offset 104 is not a direct child of <revision>
exit=1

```

### X7 <revision> with no <text>

```
xsa: error: corpus: malformed XML: page 'No text' has no <text> element
exit=1

```

### X8 <page> with no <revision>

```
xsa: error: corpus: malformed XML: page 'No revision' has no <revision>
exit=1

```

### X9 two <title> elements in one page

```
xsa: error: corpus: malformed XML: page has two <title> elements (second at offset 94)
exit=1

```

### X10 empty stream (valid bz2, zero bytes)

```
xsa: error: corpus: truncated or malformed XML: no <mediawiki> root element in the stream
exit=1

```

### X11 valid bz2, <mediawiki> with no pages at all

```
xsa: error: corpus: the dump yielded 0 documents; refusing to emit an empty corpus
exit=1

```

### X12 <mediawiki> left unclosed (valid bz2)

```
xsa: error: corpus: truncated or malformed XML: stream ended at offset 58 with <mediawiki> still open
exit=1

```

### X13 CDATA inside <text>: bytes are content, copied verbatim (no guessing)

```
exit=0
  corpus.bin=b'CD\n<![CDATA[raw]]>\x1e'

```

### X14 stdin ('-' = the compressed stream on stdin)

```
xsa corpus: docs=5 corpus_bytes=268 separators=5 separator=0x1e seps.bin=76 titles.bin=134 sha256(corpus.bin)=f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/extra/x14/MANIFEST.json
exit=0
  stdin corpus.bin == file-mode corpus.bin
  dump_path='-' dump_sha256=None docs=5 separators=5
$ truncated stream on stdin
  xsa: error: corpus: decompressor failed (exit status: 2): truncated or invalid bzip2 stream; refusing to emit a short corpus
```

### X15 anchors at scale: 200 synthetic pages (stride 64 -> anchors at docs 0/64/128/192) + rank_1E

```
  generated 46438 bytes of XML
xsa corpus: docs=200 corpus_bytes=23025 separators=200 separator=0x1e seps.bin=880 titles.bin=2820 sha256(corpus.bin)=f29a98235c9e47027d01902b2bc82f52515056f1336b311f9d14b538a89704cd
xsa corpus: manifest /mnt/nvme2n1/erikg/wikicorpus/xsa/target/wc-accept/g/anchor/out/MANIFEST.json
exit=0
  doc_count=200 corpus_bytes=23025 anchors=[0, 7337, 14728, 22173] (every 64th doc start)
  rank_1E(pos) from anchors+deltas agrees with a brute bisect on all 11 probes
  X15 PASS

```

### X16 journal: overhead vs raw bzip2 -dc (4.2 MB poorly-compressible dump; nothing else running on the box)

```
  xml=4246918 bytes  bz2=2909112 bytes
  bzip2 -dc alone   run1 WALL=0.18 s
  bzip2 -dc alone   run2 WALL=0.18 s
  bzip2 -dc alone   run3 WALL=0.20 s
  xsa corpus       run1 WALL=0.20 s MAX_RSS=4096 KB
  xsa corpus       run2 WALL=0.20 s MAX_RSS=4096 KB
  xsa corpus       run3 WALL=0.21 s MAX_RSS=4096 KB
  corpus.bin=3860000 seps.bin=16552 titles.bin=76020
```
## Final re-check (after the last cosmetic edit: `pub fn command(args: &[String])`)

```
binary: xsa sha256=2338d4e816e537184304073d0fefc91bcb99dea5b20fbe852ecfa4635ab14997   (final, 0x1E/byte-identity/determinism re-check)
$ W1  xsa corpus --wikipedia fixture_0x1e.ms.bz2 --out final/w1
xsa: error: 0x1E occurs inside document content (count=6 docs=5); rerun with --separator none
xsa: note: 1 of those 6 0x1E bytes came from document content (1 document(s) affected)
xsa: note: nothing was published; the incomplete sidecars keep their .partial names
  exit=1
$ W2  xsa corpus --wikipedia fixture.ms.bz2 --out final/w2a
  exit=0
$ W3  xsa corpus --wikipedia fixture.ms.bz2 --out final/w3 --separator none
  exit=0
$ W4  xsa corpus --wikipedia fixture.ms.bz2 --out final/w4 --max-bytes 130
xsa corpus: --max-bytes 130 reached: cut at document boundary after doc 3: corpus_bytes=153 docs=3
xsa corpus: docs=3 corpus_bytes=153 separators=3 separator=0x1e seps.bin=68 titles.bin=82 sha256(corpus.bin)=e60b5f3dcbc8815bcda53810f4200e2c9853f4c8ce629645a7db9eccaf3505fe
  exit=0
$ W5  second identical run into final/w2b
  corpus.bin  identical, sha256=f1257c0d3960eee0855a0df1541152bb7bfcd63e302b992375e9a0cb58b5bbac
  seps.bin    identical, sha256=c503f160d7b43a3a574bd91102cc58ad66e30c15360fdda418286d3665f3466d
  titles.bin  identical, sha256=f637a27c3cefa6ddfcbe2e8e631ca82f5223456b3e2dd5964d641c8f2fb76c82
  corpus.bin identical to the pre-refactor gate run (no byte drift from the last two edits)
  seps.bin has 0x1E count in none-mode corpus: 0
```

## Journal

Box: this worktree (`/mnt/nvme2n1/erikg/wikicorpus`); nothing of mine else running during
the timed runs, and the numbers say what shared the CPU.

- Build: `cargo build --release --manifest-path xsa/Cargo.toml` — first build 4m47s
  (it compiles the crate's bundled native stages: htslib/agc/pfp++/rpfbwt plus the
  bundled C++ tools), then ~2 s incremental. The build script verifies every shipped
  source against `xsa/SOURCES.sha256.json`, so it is regenerated with
  `tools/package_xsa_sources.py` on each source edit (the script also refreshes the
  packaged mirrors; here only the digest file changed).
- X16 overhead (4,246,918 B XML, 2,909,112 B bz2 — base64 bodies, poorly compressible):
  `bzip2 -dc > /dev/null` alone 0.18-0.20 s; `xsa corpus` end-to-end 0.20-0.21 s,
  max RSS 4 MB. Parse + emit sits inside the decoder's timing noise: the tool is
  decompression-bound, as a single pass over a bz2 dump should be. Counters for that
  run: docs=4000, corpus_bytes=3,860,000, separators=4000, seps.bin=16,552 B,
  titles.bin=76,020 B.
- X15 anchors: 200 docs, corpus 23,025 B, seps.bin 880 B (= 48 header + 4*200 deltas +
  8*4 anchors), anchors [0, 7337, 14728, 22173]; rank_1E(pos) from anchors+deltas
  matched a brute bisect on 11 probes.
- Fixture gates (W1-W5): each run is ~0.01 s wall and ~2 MB RSS (startup dominates a
  268 B corpus).

Exact fixture counters: doc_count=5, corpus_bytes=268 (0x1e mode) / 263 (none mode),
separator_count=5 (0x1e) / 0 (none), seps.bin=76 B (48 header + 5*4 deltas + 1*8
anchor), titles.bin=134 B (20 header + 86 B payload + 5*4 offsets + 5*2 lengths),
dump_sha256 taken over the *compressed* dump.

## Known limits and residual risks

1. `--separator none` leaves stray 0x1E bytes in corpus.bin as content: rank documents
   from seps.bin, never by counting 0x1E. MANIFEST reports `content_separator_bytes`.
2. Per-document scratch memory: one body is buffered in RAM (hard cap 512 MiB/doc; the
   pass aborts rather than reserving more) and the titles offset/length columns are
   held in RAM until the payload is complete (6 B/doc: ~270 MB at 45e6 docs). seps.bin
   deltas stream straight to the file, so they add nothing. Budget ~0.3-0.5 GB RSS at
   90 GB corpus scale — not measured at that scale here.
3. No XML entity decoding and no wikitext conversion: titles and bodies are the bytes
   between the tags, so `&amp;` stays `&amp;` in-band (MANIFEST `xml_entities`). No
   namespace filtering: every `<page>` is indexed, `Category:`/`Wikipedia:` titles
   included (MANIFEST `namespace_filter`).
4. A title containing a newline or 0x1E is refused (it cannot be carried in-band), as is
   a title longer than 65535 B (u16 length column) or a document longer than a u32
   delta (4 GiB).
5. Multiple `<revision>` elements in one page: the last (newest) one is indexed; the
   number of such pages is recorded as `pages_with_multiple_revisions`.
6. `<text>` is treated as PCDATA by scanning raw to `</text>` (dumps never nest
   elements there), so a literal `</text>` ends the body and CDATA inside `<text>` is
   kept verbatim (X13). CDATA outside an element is refused and a DOCTYPE internal
   subset is not supported; MediaWiki dumps contain neither.
7. pbzip2 is preferred and falls back to bzip2 when it is not on PATH (it is absent on
   this box, so every gate above exercised the bzip2 path). Both handle multistream bz2.
8. `--max-bytes` stops without validating the remainder of the stream, by design: the
   MANIFEST records `cut_at_document_boundary` and `cut_corpus_bytes`, and rule 1 is
   enforced over the emitted prefix only.
9. Determinism is verified at fixture scale (W5); the pass is single-threaded and does
   no hash iteration, but a 90 GB run was not executed here.
10. MANIFEST.json presence is the completion marker (`*.partial` names otherwise); it is
    written after the sidecars are renamed and fsynced, so a crash between the rename and
    the manifest leaves complete sidecars with no manifest.
11. The output directory is claimed in step 0, before the dump is hashed, so a mis-typed
    `--out` fails immediately instead of after minutes of hashing a 90 GB dump.
