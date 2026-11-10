# Pile document map — measurements

`xsa corpus --pile-map` turns the concatenated Pile stream (`pile.txt`,
1,307,910,802,540 B) into the sidecars `xsa query --docs` already reads, plus a
pile-specific metadata table. `xsa corpus --pile-verify` re-extracts documents
from their shard records and compares bytes.

    xsa corpus --pile-map --out DIR [--pile FILE] [--shards DIR] \
               [--limit-bytes N] [--jobs J] [--pile-sha256]
    xsa corpus --pile-verify --map DIR [--pile FILE] [--shards DIR] \
               [--samples N] [--jobs J]

Output directory:

| file | format | content |
| --- | --- | --- |
| `seps.bin` | XSEP v1 (unchanged) | document starts: u32 deltas + every-64th u64 anchor |
| `titles.bin` | XTIT v1 (unchanged) | the `pile_set_name` column — this is query's "title" |
| `meta.bin` | XPM1 v1 (new) | per document: offset u64, length u64, shard u16, record u64, set name u32, internal 0x1E u32 |
| `MANIFEST.json` | — | counts, shard identities, sidecar sha256s, per-component census, timings |

## Why offsets do not come from a 0x1E scan

Some document texts contain literal 0x1E bytes, so scanning `pile.txt` for
0x1E invents boundaries and splits documents mid-text. Offsets come from the
shard record boundaries (shards 00..29 in filename order, records in file
order, each record's JSON-unescaped `text` followed by one 0x1E); the pile is
then read once and every document is checked byte for byte.

The reconciliation asserted by every run:

    separators_in_range == documents_owning_separator + internal_0x1e_in_range

## Gates

### G0 tiny brute-force oracle

Two synthetic shards, five records: one text containing a literal 0x1E, one
empty text, one 5000-byte text, multi-byte UTF-8. Sidecars re-read by an
independent Python parser: starts, titles, offsets, lengths, shard/record
indices and internal counts all match the pile bytes.
Negative cases: a corrupted pile byte and a corrupted shard record both abort
with the document, both offsets and the first differing byte.

### G1 0x1E counter was inexact (found by the identity gate)

`(x - 0x01..) & !x & 0x80..` is a presence test, not a count: a borrow out of a
matching byte turns a following 0x01 into 0xff, whose high bit also reads as a
match. Over the first 80 GB it counted 12,892,533 instead of 12,892,529 — four
invented separators. Fixed with the exact per-byte form
`!(((x & 0x7f..) + 0x7f..) | x | 0x7f..)` in `corpus.rs` and `pilemap.rs`, with
a test over all 65,536 adjacent byte pairs.

### D1 DOC COUNT

10 GB slice (`pile.txt`, `--limit-bytes 10000000000`, jobs 8, wall 54 s):

    DOCCOUNT range_bytes=10000000000 records=1606584 internal_0x1e=446 separators=1607030 identity_ok=1 entries=1606585 partial_tail=1

independent cross-check (`/tmp/countsep`, memchr over the same range):
`SEPCOUNT file=pile.txt range=10000000000 seps=1607030` — equal.

100 GB slice (`pile-head100.txt`, sha256
`de79fe96057b604a71513770111f246ab0e936d3cf56415bcc5a43bba60bbbfd`,
`--limit-bytes 100000000000`, jobs 4, wall 4:31 incl. the file sha256, peak RSS
6.7 GB):

    DOCCOUNT range_bytes=100000000000 records=16119766 internal_0x1e=6979 separators=16126745 identity_ok=1 entries=16119767 partial_tail=1
    PILE_MAP_LIMIT limit_bytes=100000000000 covered_bytes=100000000000 trailing_bytes=0 shards_used=3 last_shard=02.jsonl.zst last_record=2076944 partial_tail=1
    SEP_INTERNAL records=16119767 internal_0x1e=6979 ratio=0.000432947 bytes=100000000000

Agrees with the independently measured expectation (records 16,119,766;
separators 16,126,745; internal 6,979) exactly. The last complete document ends
at 99,999,999,398; the remaining 602 bytes are a head-cut tail document that
owns no separator (`partial_tail=1`, `last_length=602`):

    DOCEND last_doc=16119766 last_offset=99999999398 last_length=602 covered_end=100000000000 corpus_bytes=100000000000 file_bytes=100000000000 uncovered=0

22 `pile_set_name` values; the internal 0x1E are concentrated in FreeLaw 4,733,
PhilPapers 2,006, Ubuntu IRC 221, StackExchange 18, OpenSubtitles 1 (sum 6,979).

### D2 BYTE EXACTNESS

Every document, every run: pile bytes `[offset, offset+length)` SHA-256 equal to
the shard record text SHA-256, and `pile[offset+length] == 0x1E`
(`DOCHASH docs_compared=16119767 mismatches=0`).

Sampled re-extraction from the shards (`--pile-verify --samples 200`), which
always adds documents whose text contains a literal 0x1E:

    DOCVERIFY n=400 exact=400 mismatches=0 internal_sampled=200 internal_exact=200

10 GB map, `--samples 100`:

    DOCVERIFY n=125 exact=125 mismatches=0 internal_sampled=25 internal_exact=25

### D4 QUERY

`xsa query /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 --docs /mnt/nvme2n1/erikg/pilemap100 -j 8 --limit 12 " the "`

    0	1774602	25901	PhilPapers	11046160732
    0	1774602	25065	PhilPapers	11046159896
    0	1774602	27259	PhilPapers	11046162090
    0	7875127	84098	PhilPapers	48898005641
    0	12512383	22159	PhilPapers	77662074855

(columns: pattern_id, doc_id, offset_in_doc, pile_set_name, global_offset)

Plausibility round trip — a 40-60 byte pattern lifted from a known document of a
known component, queried back through the container:

| source component | pattern | hit |
| --- | --- | --- |
| EuroParl (doc 4861) | `heir neutrality, we have reached an unac…` | `0 4861 4357 EuroParl 31548704` |
| Books3 (doc 1158) | `ompagner au bal ; aussi alla-t-elle en demand…` | `0 1158 78147 Books3 10260693` |
| Github (doc 1) | `iving Will Code</description>` | `0 1 1426 Github 14851` |
| PubMed Central (doc 54) | `ration, location and behavior and smoking ha…` | `0 54 6513 PubMed Central 222754` |
| FreeLaw (doc 51) | ` 31   Because we conclude that the evidence …` | `0 51 18114 FreeLaw 173622` |
| Wikipedia (en) (doc 10) | `at the stadium gave him a standing ovati…` | `0 10 1333 Wikipedia (en) 31925` |

French novel → Books3, court opinion → FreeLaw, `<description>` XML → Github:
the labels match the text, and the doc ids are the documents the patterns came
from.

### D3 END OF STREAM

Full file: the last document ends exactly at the file's end, nothing uncovered.

    DOCEND last_doc=210607727 last_offset=1307910801940 last_length=599 covered_end=1307910802540 corpus_bytes=1307910802540 file_bytes=1307910802540 uncovered=0

100 GB head slice: the last complete document ends at 99,999,999,398 and the
remaining 602 bytes are a head-cut tail document (`partial_tail=1`,
`last_length=602`) that owns no separator. The map covers all 100,000,000,000
bytes; it says so in `PILE_MAP_LIMIT` and in `MANIFEST.json`
(`partial_tail`, `corpus_bytes`, `limit_trailing_bytes`).

### D5 FULL FILE MAP

    xsa corpus --pile-map --pile /mnt/nvme2n1/erikg/pile.txt \
        --shards .../pile/train --out /mnt/nvme2n1/erikg/pilemap_full --jobs 30

    SEP_INTERNAL records=210607728 internal_0x1e=78715 ratio=0.000373752 bytes=1307910802540
    PILEMAP docs=210607728 corpus_bytes=1307910802540 covered=1307910802540 partial_tail=0 shards=30 seps.bin=868756928 titles.bin=3626765255 meta.bin=7160663742
    DOCCOUNT range_bytes=1307910802540 records=210607728 internal_0x1e=78715 separators=210686443 identity_ok=1 entries=210607728 partial_tail=0
    DOCHASH docs_compared=210607728 mismatches=0 separators=210686443 content_digest=ed0f93f1fbdd379a2c8c8cc255bc0adc6bee67c414ca707148423026de0c72d5
    DOCEND last_doc=210607727 last_offset=1307910801940 last_length=599 covered_end=1307910802540 corpus_bytes=1307910802540 file_bytes=1307910802540 uncovered=0

| measure | value |
| --- | --- |
| wall | 12:00.73 (`time -v`, whole pass incl. verification of all 1.31 TB) |
| peak RSS | 56,013,532 kB = 53.4 GiB |
| seps.bin | 868,756,928 B (4.12 B/document) |
| meta.bin | 7,160,663,742 B (34 B/document) |
| titles.bin | 3,626,765,255 B (17.2 B/document) |
| documents | 210,607,728 |
| separators (0x1E) | 210,686,443 = 210,607,728 framing + 78,715 internal |
| stage split | shards 556 s, pile verification 538 s, sidecars 242 s |
| peak disc | ~12 GB of sidecars, no scratch files, no position column |

The box was NOT idle during this run: other lanes' jobs were live (an `xsa
ctx-build` at ~60 GB RSS and two `emender` python experiment workers at ~100 %
CPU), so 12:00 is a contended measurement, not a quiet-box one.

Full-file sampled re-extraction from all 30 shards:

    DOCVERIFY n=400 exact=400 mismatches=0 internal_sampled=200 internal_exact=200

The internal 0x1E census over the whole Pile (sum 78,715): FreeLaw 67,288,
PhilPapers 8,891, Ubuntu IRC 2,462, StackExchange 44, Gutenberg (PG-19) 7,
Github 6 — court documents and philosophy PDFs, i.e. text extracted from
PDF/HTML with stray control bytes, not Wikipedia and not code.
