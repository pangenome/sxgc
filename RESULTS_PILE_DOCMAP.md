# The Pile, fully addressable: 210.6 M documents, verified to the byte

`xsa corpus --pile-map` over the whole corpus (`/mnt/nvme2n1/erikg/pile.txt`,
1,307,910,802,540 B), 2026-11-10.

## Result

```
doc_count                  210,607,728
separator_count            210,686,443
documents_owning_separator 210,607,728
internal_separator_bytes        78,715      (documents whose text contains a literal 0x1E)
corpus_bytes           1,307,910,802,540
shards_used 30 / 30      partial_tail false      peak_rss_kb 56,013,532
```

Independent verification (a separate scan of the whole file counting 0x1E bytes, 11 m 53 s):

```
separators_total = 210,686,443        == manifest separator_count, exactly
identity: documents_owning_separator + internal_separator_bytes == separator_count
```

## What it costs

```
seps.bin      868,756,928 B   (4.1 B/doc: u32 start deltas + anchors every 64)
meta.bin    7,160,663,742 B   (34 B/doc: offset, length, shard, record, component, internal-0x1E)
titles.bin  3,626,765,255 B   (17 B/doc: interned component name per document)
            ─────────────────
           11,656,185,925 B = 0.009 n     (0.9 % of the corpus)
```

Mean document: 6,210 bytes. The map is cheap because it is a catalogue, not a copy: no text is written,
no text is copied, and the corpus is read once.

## Why offsets come from the shards, not from scanning for 0x1E

`pile.txt` is `for shard 00..29: for record: text_bytes + one 0x1E`, verified byte-identical against the
shards (`cmp` on 575 MB of reconstruction, and 100/100 sampled documents afterwards). 78,715 documents
contain a literal 0x1E in their text, so a scan-based map would invent 78,715 boundaries and split those
documents mid-text. The scan is still useful as a cross-check, with the exact expected value:
`separators == documents_owning_separator + internal_separator_bytes`.

## How it was verified

- 10 GB prefix: 1,606,585 documents; separators 1,607,030; difference 445 = internal 0x1E.
- 60 GB prefix: 9,660,020 documents; separators 9,663,620; difference 3,601.
- full corpus: the table above, with an independent 0x1E scan matching exactly.
- an independent reader (`tools/pile_doc.py`, written against the XSEP/XTIT specs rather than against the
  writer) parses all three scales, resolves documents, and reproduces document text from `pile.txt`.

## What it unlocks

- `xsa query --docs <map>` resolves a hit to `doc_id`, `offset_in_doc` and `pile_set_name`.
- `project(ctx, docset)`: restrict any operation to a set of documents.
- the semantic experiment: take one document, rank documents by exact-match statistics weighted by rarity,
  and read the neighbourhood.
- reproducible sampling for training: the permutation becomes a list of document ids rather than a `shuf`
  of bytes, so the corpus never has to be rewritten to be reordered.

## History worth remembering

Two deadlocks had to be fixed before this ran at all, both hidden behind "it has been running for hours":
a second decompressor writing into a pipe nobody drained, and a worker that stopped reading at a byte
limit and then waited on a decompressor blocked writing into the full pipe. Both were diagnosed from
`/proc/<pid>/io` (rchar 9 KB after three hours) and per-thread `wchan` (workers in `do_wait`, main in
`futex_wait`), not from elapsed time. The fix made the same job finish in about five minutes, and the tool
now prints `PILE_MAP_PROGRESS` so a future stall is visible in the log.
