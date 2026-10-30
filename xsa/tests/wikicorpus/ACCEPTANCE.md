# ACCEPTANCE — `xsa corpus` (Wikipedia dump -> corpus + document sidecars)

Tool: `xsa corpus --wikipedia <dump.xml.bz2|-> --out <dir> [--max-bytes 90G] [--separator 0x1e|none]`
Source: `xsa/src/corpus.rs` (new module), dispatch in `xsa/src/main.rs`.
Build: `cargo build --release --manifest-path xsa/Cargo.toml`.

Fixture (committed): `xsa/tests/wikicorpus/fixture.xml` — 2304 bytes of hand-written
MediaWiki-shaped XML, 5 pages, real `<siteinfo>`, namespaces table, contributor
blocks, an escaped-entity body, a `Category:` (namespace 14) title, an empty
`<text></text>`, a `<text deleted="deleted" />` page, and a multi-line body with a
literal tab, trailing spaces and `&amp;`. It is fed to the tool through
**multistream-shaped** bz2 files (3 concatenated `bzip2 -c` streams), the shape
real `pages-articles-multistream` dumps have.
`xsa/tests/wikicorpus/fixture_0x1e.xml` is the identical file with one raw 0x1E
byte inside page 3's `<text>` (the rule-1 collision case).

All commands below were run with `timeout 900`; nothing larger than a few MB was
processed and no Wikipedia data was downloaded.

## W1 — 0x1E inside a body must be REFUSED (fatal, nothing published)

```
xsa: error: 0x1E occurs inside document content (count=6 docs=5); rerun with --separator none
xsa: note: 1 of those 6 0x1E bytes came from document content (1 document(s) affected)
xsa: note: nothing was published; the incomplete sidecars keep their .partial names
exit=1
$ ls w1
  total 0
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:17 corpus.bin.partial
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:17 seps.bin.partial
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:17 titles.bin.partial

### W2  clean fixture: byte-exact corpus + sidecars
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


## W1  0x1E inside a body must be REFUSED

```
xsa: error: 0x1E occurs inside document content (count=6 docs=5); rerun with --separator none
xsa: note: 1 of those 6 0x1E bytes came from document content (1 document(s) affected)
xsa: note: nothing was published; the incomplete sidecars keep their .partial names
exit=1
$ ls w1
  total 0
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:25 corpus.bin.partial
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:25 seps.bin.partial
  -rw-rw-r-- 1 erikg erikg 0 Oct 30 13:25 titles.bin.partial

```

## W2  clean fixture: byte-exact corpus + sidecars

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

## W3  --separator none

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

## W4  --max-bytes cuts at a document boundary

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
