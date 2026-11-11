# CORPUS V2 — the gates

`CORPUS_V2.md` is the specification of record; this file records the gate run
that proved the implementation against the real Pile shards. Every quoted
output below is the literal output of the named command, run on **this** box on
2026-11-11 (worktree `wt/corpus-v2`, branch `feat/corpus-v2`). Box: 2×AMD
EPYC 7713 (256 threads), 1007 GiB RAM; **other lanes were running during the
run** (load average 31–46 of 256 through the session), so the walls below are
generous-side measurements, not clean-box numbers.

Source set: `shards2/` — a directory with hardlinks to the Pile train shards
`00.jsonl.zst` (15,240,542,576 B) and `01.jsonl.zst` (15,213,826,121 B) plus
the release's `SHA256SUMS.txt` (zstd refuses symlinks; hardlinks are same-FS).

Prefixes (one MANIFEST.json per directory):

| corpus | construction | doc_count | corpus_bytes | tables |
| --- | --- | --- | --- | --- |
| `c500/p` | `--v2-meta --limit-bytes 524288000` | 87,219 | 524,255,378 | `.starts` 359,828 B; `.docs` 2,965,932 B; no `.order` (source order) |
| `c500r/p` | `--v2-reorder --order-seed 20261111` from `c500/p` | 87,219 | 524,255,378 | tables 3,674,664 B total incl. `.order` 348,904 B |
| `c150/p` | `--v2-meta --limit-bytes 150000000` | 25,455 | 149,977,961 | `.starts` 105,052 B; `.docs` 865,956 B |

`excluded.empty_text_records`: c500 = 1, c150 = 0 (the rule of CORPUS_V2.md 3.2).

---

## V2 — define a 500 MB corpus, metadata only

```
$ xsa corpus --v2-meta --shards shards2 --out c500/p --limit-bytes 524288000
V2_META prefix=c500/p docs=87219 corpus_bytes=524255378 excluded_empty=1 order=source starts_bytes=359828 docs_bytes=2965932 order_bytes=0
V2_META_LIMIT limit_bytes=524288000 corpus_bytes=524255378 boundary_aligned=true shards_used=1 last_admitted_shard=0 last_admitted_record=87219
V2_META_MANIFEST c500/MANIFEST.json
```

wall **1.11 s**, peak RSS 12 MiB (GNU time), one sequential decompressor
(`shards_used=1`: the budget cut inside shard 00 of the 2-shard source).
**No text bytes were written**: 3.3 MB of tables describe a 500 MB corpus.

## V3 — the tables are the corpus

```
$ xsa corpus --v2-verify --prefix c500/p --samples 200
V2_VERIFY prefix=c500/p docs=87219 corpus_bytes=524255378 order=source
V2_CACHE bin=present bytes=524255378==corpus_bytes
V2_TABLES deltas_positive=all sum_lengths=524255378==corpus_bytes starts_docs_agreement=ok bijection=ok
V2_SAMPLE docs_sampled=200 shards_touched=1 compare_to=bin+shards reference=none
V2_BYTES sampled=200 byte_identical=200 mismatches=0
V2_VERIFY_OK prefix=c500/p docs=87219 sampled=200 corpus_bytes=524255378
```

wall 0.78 s, peak RSS 23 MiB. 200 sampled documents re-extracted from their
shard records and byte-compared against the concatenation at
`[start, start+len)` — all 200 identical. `sum(lengths) == corpus_bytes`,
deltas positive, starts/docs pair agreement, all asserted.

## V4 — THE FLAGSHIP: reordering writes tables, not bytes

```
$ xsa corpus --v2-reorder --from c500/p --out c500r/p --order-seed 20261111
V2_REORDER from=c500/p out=c500r/p docs=87219 corpus_bytes=524255378 seed=20261111 tables_bytes=3674664 wall_ms=112 bytes_moved=0
```

**112 ms, zero corpus bytes moved, no shard opened, no `.bin` produced** — the
reordered corpus is 3.5 MB of tables. Then the bijection / multiset / identity
gate against the source as reference:

```
$ xsa corpus --v2-verify --prefix c500r/p --samples 200 --reference c500/p
V2_VERIFY prefix=c500r/p docs=87219 corpus_bytes=524255378 order=file
V2_CACHE bin=absent (optional cache; the tables are the corpus)
V2_TABLES deltas_positive=all sum_lengths=524255378==corpus_bytes starts_docs_agreement=ok bijection=ok
V2_REFMULTISET docs=87219 multiset=identical corpus_bytes src=524255378 ref=524255378
V2_SAMPLE docs_sampled=200 shards_touched=1 compare_to=shards reference=given
V2_BYTES sampled=200 byte_identical=200 mismatches=0
V2_VERIFY_OK prefix=c500r/p docs=87219 sampled=200 corpus_bytes=524255378
```

* bijection: asserted on read of `.order` (sorted-copy check), `bijection=ok`.
* same multiset: `(shard, record, length)` sorted-compare — `multiset=identical`.
* same total bytes: 524,255,378 both.
* document k of the ordered view == document perm[k] of the source: each of
  the 200 sampled documents re-extracted from the shards and byte-compared
  against the REFERENCE's `.bin` at `reference.starts[order[k]]` — 200/200
  identical (the gate asks ≥100).
* `--v2-locate` cross-check: the same global offsets resolve into completely
  different documents in `c500r`, each carrying `source_doc_id` provenance:

```
$ xsa corpus --v2-locate --prefix c500/p --offset 0,524255377,300000000
V2_LOCATE offset=0 doc_id=0 offset_in_doc=0 length=13424 shard=00.jsonl.zst record=0 component=Pile-CC source_doc_id=0
V2_LOCATE offset=524255377 doc_id=87218 offset_in_doc=2329 length=2330 shard=00.jsonl.zst record=87219 component=USPTO Backgrounds source_doc_id=87218
V2_LOCATE offset=300000000 doc_id=49695 offset_in_doc=15478 length=51577 shard=00.jsonl.zst record=49695 component=PubMed Central source_doc_id=49695
$ xsa corpus --v2-locate --prefix c500r/p --offset 0,524255377,300000000
V2_LOCATE offset=0 doc_id=0 offset_in_doc=0 length=736 shard=00.jsonl.zst record=32458 component=Pile-CC source_doc_id=32458
V2_LOCATE offset=524255377 doc_id=87218 offset_in_doc=6699 length=6700 shard=00.jsonl.zst record=85852 component=Pile-CC source_doc_id=85851
V2_LOCATE offset=300000000 doc_id=48879 offset_in_doc=418242 length=467051 shard=00.jsonl.zst record=41763 component=Books3 source_doc_id=41763
```

The `--order FILE` (XORD v1) arm ran too: reordering `c500` by the
seed-generated `.order` file gave `wall_ms=82 bytes_moved=0` and the same
verify result with 150 samples (`byte_identical=150 mismatches=0`).

## V5 — materialize the view and index it through the V2 tables

```
$ xsa corpus --v2-materialize --prefix c500/p
V2_MATERIALIZED prefix=c500/p bin=c500/p.bin docs=87219 bytes=524255378 sha256=841e7942acd206484195c94cdc5f7e716f20e2f26572b1a0a138c84feea16bba (the cache; delete and regenerate any time)
```

wall **3.78 s**, peak RSS 21 MiB; `|p.bin| == corpus_bytes` exactly (one
bounded window of decompressors, texts written straight into place, permuted
corpora assemble the same way).

Index over the 150 MB view (`--docs-map`, tape route; c150 corpus built the
same way, `docs=25455 corpus_bytes=149977961`):

```
$ xsa build --tape --input c150/p.bin --docs-map c150/p --scratch S --threads 24 --memory-gb 16 --no-trie --no-lcp-runs --with-chi
V2_CONTAINER_PUBLISHED path=S/p.sxi2 bytes=118853984
XSA_BUILD_DONE chi=44021263 pipeline=v2tape container=S/p.sxi2
```

wall 1:33, and the journal line the query side reads back:

```
CONTAINER_DOCS_V2 prefix="c150/p" starts=p.starts docs=25455 corpus_bytes=149977961 order=source
RAW_BYTES n=149977961 terminal=virtual-END transformation=none
```

The same build over the full 500 MB view: container 399,836,912 B, wall 2:32,
`chi=150530124`, `CONTAINER_DOCS_V2 ... docs=87219 corpus_bytes=524255378`.

Queries resolve doc_id / offset_in_doc **through the V2 tables** (explicit
`--docs` and the journal-recorded fallback, and `--context` over the cache):

```
$ xsa query S/p.sxi2 "the present invention" --docs c150 --limit 8 --output tsv
QUERY_DOCS source=docs docs=25455 corpus_bytes=149977961
0	21764	10339	USPTO Backgrounds	129992121
0	21764	14101	USPTO Backgrounds	129995883
...
$ xsa corpus --v2-locate --prefix c150/p --offset 129992121
V2_LOCATE offset=129992121 doc_id=21764 offset_in_doc=10339 length=15958 shard=00.jsonl.zst record=21764 component=USPTO Backgrounds source_doc_id=21764
$ xsa query S/p.sxi2 "the present invention" --limit 3 --output tsv
QUERY_DOCS source=recorded docs=25455 corpus_bytes=149977961
$ xsa query S/p.sxi2 "the present invention" --docs c150 --context 40 --limit 2
0	21764	10339	USPTO Backgrounds	129992121	pound into the solvent) is complete.\nIn the present invention N-[N-[5-[4-(aminoi
```

The `--v2-locate` of the query's global offset reproduces the query's
`doc_id=21764 offset_in_doc=10339` — the same tables, two readers — and the
bytes at `p.bin[129992121..]` are the queried text.

**The chi values above are NOT comparable to pile.txt-based containers.** The
V2 byte stream is the raw concatenation without 0x1E framing, so the text
differs from pile.txt's stream at every former separator position; chi, counts
and every content-dependent measurement differ BY CONSTRUCTION and are never
compared across corpora. The document map of the V1 corpus keeps describing
pile.txt unchanged (CORPUS_V2.md 8).

## V6 — no regression

`cargo test --release` in `xsa/`: **94 passed, 0 failed** (1 ignored,
pre-existing), including every pre-existing qlite/chain/corpus test and the
new corpus-v2 suites (tiny-scale oracle with adversarial documents: 0x1E
inside text, separator-at-the-tail documents; an independent hand-spelled
XCS2/XPM1 fixture for the query reader — which caught and fixed a u16-column
read as u32 before any real corpus ran). Merged to main and pushed; see
REPRODUCE.md 11 for the corpus-v2 route.
