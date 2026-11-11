# CORPUS V2 — the separator-free corpus format

Status: **specification of record** for all FUTURE datasets. `pile.txt` is NOT
remade under this format (see "Migration note" at the end — it must never be).

## 0. The principle (settled, from NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md)

At vocabulary 256 no in-band delimiter can be safe by construction: whatever byte
one picks, some document text somewhere contains it (measured in pile.txt:
thousands of literal 0x1E inside the first 100 GB of Pile texts). A format whose
boundaries are inferred from the bytes is therefore a format whose boundaries are
sometimes wrong, and every downstream thing built on the boundary (doc_id
ranking, partitioning, sampling) inherits the error silently.

Therefore in V2 **THE FRAMING BYTE DOES NOT EXIST**. The corpus byte stream is
the raw concatenation of document texts — nothing between them, no header, no
padding, no terminal byte — and document boundaries live ONLY in metadata.
`n` (the byte length of the corpus) is exactly the sum of document lengths.
No inference is possible because there is nothing to infer: a position is in
document `k` iff `starts[k] <= pos < starts[k] + lengths[k]`, and `starts`,
`lengths` are read from the tables, never scanned out of the text.

## 1. The corpus is the metadata; the bytes are a cache

**A V2 corpus is the quadruple (sources, starts, docs, order).** The four
tables fully determine the virtual byte stream — document-level operations
(document sampling, permutation, restriction to a subset, catalogue, provenance
lookup) never read a single text byte. This follows the operator directive
already in force in this repo ("never materialize the text": the 1.4 TB
`h466_rl.txt` expansion was deleted; the pipeline reads the compressed AGC
archive). A format that *requires* an uncompressed on-disc copy of a 1.2 TB
corpus would contradict it.

`<name>.bin` — the materialized concatenation — is therefore **an OPTIONAL
cache**, not part of the corpus definition:

* it is written only when something wants a plain byte file (a training
  loader, or `xsa build` over a source that is not itself random-access);
* deleting it never loses information: it can always be regenerated exactly
  from (sources, starts, docs, order);
* it carries no framing and no terminal byte. `wc -c <name>.bin` must equal
  `corpus_bytes` in `<name>.starts` exactly.

### 1.1 The SOURCE ACCESS INTERFACE (the contract bytes satisfy)

The only consumer that must read corpus bytes at scale is the index build, which
needs **random reads**: the suffix comparison walk reads corpus bytes at
arbitrary positions. That requirement is a contract, not a file:

```
access.seek(pos: u64, len: u64) -> &[u8]      // bytes [pos, pos+len) of the
                                              // VIRTUAL corpus, or fail loudly
```

with the rules: bounded windows (no full-corpus requirement), positions may be
arbitrary and unsorted, and the bytes returned must be exactly the concatenation
the tables describe. Acceptable source kinds:

1. **Raw file** — the materialized `<name>.bin`, or the already-existing
   `pile.txt` for the V1 corpus (seek-based, trivially compliant). This is the
   only kind implemented today.
2. **Block-compressed archive with a block index** (AGC-style): compressed on
   disc; a block is decompressed into a bounded window on demand. RESERVED
   EXTENSION: the interface is the contract, a new `source_kind` in the
   manifest is the entry point; not implemented at this time.
3. **Plain `.jsonl.zst` shards are NOT random-access capable.** The Pile's
   shipped shards must be consumed sequentially; building over them requires
   either an existing compliant raw file (for the Pile: keep `pile.txt` — it
   already exists, is hash-bound in its manifest, and is the trained-on
   corpus) or a one-time conversion to kind 1 or 2. The V2 tooling therefore
   never claims shards as an index-time source; it uses them only at
   construction time (streaming) and verification time (sequential re-read).

## 2. File layout (byte for byte; everything little-endian)

A V2 corpus named `<name>` lives in one directory as five files. `<name>` is a
prefix (its directory need not be dedicated, but `MANIFEST.json` names the
corpus it describes, so one manifest per directory):

```
<name>.bin       OPTIONAL materialization (§1): the raw concatenation of the
                 document texts, in corpus order. No separator, no header, no
                 padding, no terminal byte. DELETE-AND-REGENERATE-ABLE.
<name>.bin.sha256  written by --v2-materialize, sha256sum-comparable format
                 ("<hex>  <name>.bin\n"). Its presence means the cache exists.
<name>.starts    document starts (XCS2 v1) — REQUIRED, see §6.
<name>.docs      per-document metadata (XPM1 v1 shape) — REQUIRED.
<name>.order     the permutation as data (XORD v1) — OPTIONAL: ABSENT means
                 source order (identity).
MANIFEST.json    shas of everything + the construction command + source
                 identity.
```

### 2.1 `<name>.starts` — XCS2 v1

Same shape as XSEP v1 (48-byte header, u32 delta column, u64 anchor sample at
stride 64), so the existing rank/select code over starts can be shared; only
the magic and flags differ, and the delta semantics are separator-free:

```
[ 0.. 4)  b"XCS2"
[ 4.. 8)  u32 version = 1
[ 8..12)  u32 flags   = 0        (bit0: 1 = "a separator byte follows every
                                  document" — XSEP semantics; ALWAYS 0 here)
[12..16)  u32 anchor_stride = 64
[16..24)  u64 doc_count
[24..32)  u64 corpus_bytes     == sum(lengths) == |.bin| if present
                              (NOT sum(deltas): the deltas end at the last
                              document's START — see the invariant below)
[32..40)  u64 anchor_count    == ceil(doc_count / 64)
[40..48)  u64 base = 0        (start of document 0; always 0 today)
then doc_count * u32 LE deltas: d[0] = start[0] - base = 0,
     d[i] = start[i] - start[i-1] = length(doc i-1) for i >= 1
then anchor_count * u64 LE absolute starts of every 64th document
     (start of doc k*64, k = 0.. anchor_count-1)
```

Invariants (asserted by every reader): `d[0] == 0`; `d[i] > 0` for all `i >= 1`
(every document has length >= 1 — a zero-length document is not representable
and is refused at construction, see §3.2); **`sum(d[i]) == start[doc_count-1]`**
(the deltas carry every document BOUNDARY; the corpus END is not a boundary
entry — this is exactly XSEP v1's column semantics, and it is why the
rank/select code is shared). Therefore `start[doc_count-1] < corpus_bytes`,
and the terminal length `corpus_bytes - start[doc_count-1]` is the length of
the LAST document. That terminal length is pinned by `.docs` (§2.2), which is
REQUIRED in V2: every reader loads the pair and asserts the agreement
(`offset[k] == start[k]` for all k, rows tile,
`sum(lengths) == corpus_bytes`), so `.starts` is never trusted alone and the
pair is self-verifying. Anchors agree with the accumulated deltas; file
length is exactly `48 + 4*doc_count + 8*anchor_count`.

`doc_id` of a corpus position `pos`: the `k` with `start[k] <= pos <
start[k]+length[k]`, found by anchor binary search + at most 63 delta additions
(XSEP rank/select, unchanged).

### 2.2 `<name>.docs` — XPM1 v1 shape, in CORPUS order

Row `k` describes corpus document `k` (i.e. the `k`-th document of the
concatenation, NOT the `k`-th source record when an order is present — see §4).
Identical to the XPM1 v1 layout `xsa corpus --pile-map` writes for `meta.bin`
(68-byte header; component-name blob + offsets + lengths; shard-name blob +
offsets + lengths; then one 34-byte row per document):

```
row k: u64 offset (== start[k], same number as .starts says),
       u64 length,
       u16 shard (index into the shard-name table),
       u32 component (index into the component-name table, interned),
       u64 record (record index inside the shard, 0-based, blank lines
                   not counted — the pilemap convention),
       u32 internal_0x1e — RESERVED, ALWAYS 0 in V2 (a V1 concept; readers
                           must not use it).
```

Header: `b"XPM1"`, u32 version=1, u64 doc_count, u64 corpus_bytes, u64
shard_count, u64 name_count, u64 name_payload, u64 shard_payload, u32
row_bytes=34, u64 partial_tail=0 (always 0 in V2 — V2 never cuts a document;
§5). Readers assert `offset[k] == start[k]`, `sum(lengths) == corpus_bytes`,
`tiled` rows (`offset[k] + length[k] == offset[k+1]`), `length > 0`.

### 2.3 `<name>.order` — XORD v1, OPTIONAL

ABSENT = source order (corpus document `k` IS source document `k`; identity
permutation). Present = the permutation as data:

```
[ 0.. 4)  b"XORD"
[ 4.. 8)  u32 version = 1
[ 8..12)  u32 flags: bit0 = 1 when generated from --order-seed (the generator
                     spec is then in MANIFEST.json; the file is still the
                     truth — see §4)
[12..20)  u64 doc_count
[20..28)  u64 seed (0 when the permutation was supplied as a file)
[28..  )  doc_count * u32 LE: src_id[k] = the SOURCE document id placed at
          corpus position k
```

Invariants: `src_id` is a bijection of `0..doc_count` (checked by the reader:
sorted copy equals identity); file length is exactly `28 + 4*doc_count`.

**The point: order is policy, never a shuf of bytes.** Reordering a V2 corpus
writes tables only — zero corpus bytes move (see §4.3).

### 2.4 `MANIFEST.json`

Written once by the constructing command; identifies the corpus and pins every
table: `tool`, `manifest_version`, `kind: "corpus-v2"`, `name`, `doc_count`,
`corpus_bytes`, `order` (kind: `source` | `file` | `seed`, plus `seed`,
`algorithm`, `permutation_sha256` when applicable), `limit_bytes` (when given),
`source_access` (kind + the shard set: path, shard names, expected sha256 from
the release's `SHA256SUMS.txt`), `construction` (the exact command),
`sidecars` (bytes + sha256 of `.starts`, `.docs`, `.order`), `excluded`
(`empty_text_records` — see §3.2), timings, peak RSS. The optional `.bin` cache
is NOT in the manifest (it is regenerable); its identity lives in
`<name>.bin.sha256`.

## 3. Construction

### 3.1 `xsa corpus --v2-meta --shards DIR --out PREFIX [--limit-bytes N] [--order FILE | --order-seed S]`

The PRIMARY, always-legal operation: define the corpus. Streams the Pile shards
(pilemap's proven reader discipline: one `zstd -dc` decompressor alive at a
time; the reader thread closes its end of the pipe before `wait()` so a full
pipe can never wedge the reap; a shard stopped early because the budget is
reached is killed on purpose and its SIGPIPE/kill exit is accepted; a shard
that fails when it was NOT stopped on purpose is a hard error — never emit
tables from a truncated decode). Writes `.starts`, `.docs`, `.order` (when
reordered), `MANIFEST.json`. **Writes NO text bytes.** Costs one sequential
decompress per shard it actually needs, and megabytes of disc.

### 3.2 Document admission rules (construction-time, loud)

* every document must have length >= 1. A record with empty `text` cannot be
  represented (its delta would be 0 and the starts would collide); such
  records are EXCLUDED, counted, and reported (`excluded.empty_text_records`)
  — exclusion is part of the corpus definition and the docs table is the truth.
* a document longer than 2^32-1 bytes is refused (u32 delta column).
* blank lines in the shard stream are refused (a record would be silently
  missing — the pilemap rule).
* a document containing literal 0x1E (or any other byte) is fine. That is the
  whole point: nothing is in-band.

### 3.3 `--limit-bytes N` — a budget, always cut at a document boundary

Documents are admitted while the running total is < N; the first document
that would not fully fit ends the corpus. The corpus therefore never ends
mid-document, and `partial_tail` does not exist in V2 (the XPM1 column is
written 0). The manifest reports both the budget and the achieved
`corpus_bytes` (<= N).

### 3.4 `xsa corpus --v2-materialize --prefix P [--shards DIR] [--jobs J]`

EXPLICIT, OPTIONAL: write the `<prefix>.bin` cache (+ its `.bin.sha256`) by
re-extracting every document from the shards in corpus order. One decompressor
per touched shard under pilemap's lifetime rules (a bounded window of at most
`jobs` children, one reader thread per child — never N writers into one
reader; the pipe closes before `wait()`; a child stopped early on purpose is
killed and its SIGPIPE/kill exit is accepted), records in file order, each
text seek-written to its known corpus start (starts are known from the tables
before the first byte moves, so a permuted corpus assembles in place with the
same single pass). Verifies by construction: every corpus position is written
exactly once and the file size must equal `corpus_bytes`; the sha256 is
computed by reading the file back. Deleting the cache never loses information
— rerun this to regenerate.

### 3.5 `xsa corpus --v2-reorder --from P1 --out P2 (--order FILE | --order-seed S)`

THE FLAGSHIP: reorder an existing corpus by writing TABLES ONLY. Reads P1's
tables, composes the new permutation onto P1's order (see §4.2), rewrites
`.starts` deltas for the new sequence, reorders `.docs` rows, writes `.order`,
and a manifest naming P1 as the source. No shard is opened, no text byte is
read or written, no `.bin` is produced; reordering 500 MB of documents costs
milliseconds and megabytes.

## 4. Order semantics

### 4.1 What the permutation means

`order[k] = src_id` places SOURCE document `src_id` at corpus position `k`.
"Source document id" counts the documents of the construction-time source
sequence (shards in filename order 00,01,..., records in file order, after
§3.2 exclusions). `doc_id` in query output is the CORPUS position `k`
(the position in `.starts`/`.docs`), matching V1's doc_id semantics; the
provenance of corpus document `k` is in `.docs` row `k` (shard, record).

### 4.2 Composition (reordering a reordered corpus)

`--v2-reorder` over P1 composes: P2's `src_id[k] = P1.src_id[perm2[k]]`
(P1.src_id = identity when P1 has no `.order`). The `.order` file always
references CONSTRUCTION-TIME source ids, never the intermediate corpus's
positions, so chains of reorders stay flat and the shard/record provenance in
`.docs` row k always names the true origin record.

### 4.3 The seed generator (reproducible elsewhere, exactly)

`--order-seed S` generates the permutation with this algorithm and nothing
else (also recorded in MANIFEST.json as `algorithm`):

* Generator: **splitmix64**. State `x = S`. Each draw: `x = x.wrapping_add(0x9E3779B97F4A7C15)`;
  `z = x`; `z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9`;
  `z = (z ^ (z >> 27)) * 0x94D049BB133111EB`; result `z ^ (z >> 31)`.
* Permutation: Fisher–Yates, descending. `perm[i] = i` for all i; for
  `i = n-1` down to `1`: `u = splitmix64()`; `j = (u * (i+1)) >> 64` computed
  in u128 (Lemire's unbiased widening multiply), giving `j` in `0..=i`; swap
  `perm[i]`, `perm[j]`.

A seed-generated order is always MATERIALIZED as `<name>.order` by our tools
(the manifest also records seed + algorithm), so no downstream consumer ever
has to reimplement the generator. The spec permits a published corpus to carry
only the seed + algorithm (it is reproducible), but our writer chooses data
over reimplementation.

### 4.4 Bijection is asserted, never assumed

`--order FILE` (XORD v1): the file must be a bijection of `0..doc_count` —
checked by sorting a copy; anything else is refused. Same check on read of
any `.order`.

## 5. Cutting a V2 corpus (cluster partitioning)

A byte-framed stream cannot be cut safely: any cut point not exactly at a real
boundary splits a document, and with in-band framing you cannot even FIND the
real boundaries (the Pile's own texts contain the separator). V2 cuts
**exactly**, because boundaries are a table lookup:

To partition into target-size pieces: for each piece boundary target `B`,
find `k = ` the largest `k` with `start[k] <= B` (anchor binary search + <=63
deltas — the same rank/select as §2.1), and cut at `start[k]`: the piece is
documents `[k0, k1)`. `--limit-bytes` (§3.3) is this rule at construction
time. Each piece is itself a valid V2 corpus: take the `.docs` rows `[k0,k1)`,
re-base `.starts` deltas (`d[0] = 0`, the rest unchanged — deltas are lengths
and never depend on the cut), write a manifest. The cut never invented a
boundary because there was no boundary to invent.

## 6. The index side: `xsa build`, `--docs-map`, and the docstarts member

* `xsa build --tape --input <corpus> --docs-map <PREFIX>` accepts a V2 corpus.
  The build reads raw bytes (`--tape`: END is virtual, no terminal byte
  required — this is the V2 route; a V2 corpus has no terminal separator and
  the snap route would correctly refuse it). `n` needs no separator
  accounting: `n_rows = file_bytes + 1` (the structural END), and the file
  size must equal `corpus_bytes`.
* `--docs-map` points at the V2 prefix. The build reads `<prefix>.starts`,
  asserts `corpus_bytes == |input|` and the §2.1 invariants, and journals:
  `CONTAINER_DOCS_V2 prefix=... starts=<name>.starts docs=N corpus_bytes=B order=source|file|seed`.
  With no `--docs-map`, the build behaves exactly as before (V1 sibling
  `seps.bin` discovery unchanged).
* **The query side resolves doc_id/offset_in_doc through the same tables.**
  `xsa query <container> <pattern> --docs <prefix-dir>` opens the V2 pair
  (detected by the XCS2 magic; the journal's `CONTAINER_DOCS_V2` line is the
  recorded fallback), asserts the starts/docs pair agreement at open, and
  emits `doc_id`, `offset_in_doc` and the component name in the title column
  for every hit. `--context` reads the optional `<prefix>.bin` cache; a V2
  snippet never strips a separator (there is none) and never splits on 0x1E
  in text.
* **The docstarts member is REQUIRED for V2 corpora.** There is no fallback:
  a V2 corpus without its `.starts` is unindexable for document-aware work — by
  design, because the bytes contain no recoverable boundary information at
  all. (`--docs-map` and the in-container docstarts member are THE SAME DATA:
  V2 makes the sidecar and the member agree by construction — the member is
  written from the sidecar the build just validated, so they cannot disagree.
  The container-side member reader is the structural-docs lane's surface;
  the shared XCS2/XSEP shape is why that is a small change.)

## 7. Verification

`xsa corpus --v2-verify --prefix P [--shards DIR] [--samples N] [--reference P1] [--jobs J]`:

1. Table invariants: §2.1/§2.2/§2.4 (deltas positive,
   `sum(lengths) == corpus_bytes` with the starts/docs pair agreement, rows
   tile, anchors agree, bijection when `.order` present, file lengths).
2. Cache check when `.bin` exists: `|.bin| == corpus_bytes`.
3. Byte identity: `>= N` (default 200) sampled corpus documents are re-extracted
   from their shard records (one decompressor per touched shard, the §3.1
   discipline) and compared BYTES against the concatenation at
   `[start, start+len)`:
   * against `<prefix>.bin` when the cache exists;
   * against `--reference P1`'s `.bin` at P1's start of the SAME source
     document (`P1.starts[order[k]]`) — this is the reordered-corpus gate:
     document k of P is byte-identical to document perm[k] of P1.
   A corpus with no `.bin` and no `--reference` still verifies its tables and
   every sampled document's LENGTH and provenance against the shards.

`xsa corpus --v2-locate --prefix P --offset O[,O...]` resolves a global offset
through the V2 starts to `doc_id, offset_in_doc, length, shard, record` (+
`source_doc_id` when ordered) — the query-side resolution primitive, for
consumers not yet taught XCS2. `xsa query <container> <pattern> --docs
<prefix-dir>` (§6) is the same resolution wired into the query output: TSV
`doc_id` and `offset_in_doc` columns come from the V2 tables, with the
component name in the title column, and `--context` windows are cut at
DOCUMENT boundaries read from `<prefix>.starts` — no separator byte exists to
strip, and text containing 0x1E passes through escaped, never split.

## 8. Migration note — when to use V2, and why pile.txt is never remade

**Use V2 for every FUTURE corpus**: any dataset whose boundaries must be exact
(all of them), anything that may be reordered, partitioned or catalogued at
document level (the tables are the whole cost), and anything built from
sources where separator bytes occur in content (the Pile, web crawls, code).

**`pile.txt` must never be remade.** It is the corpus that was indexed and
trained on; every registered experiment holds offsets INTO it. Its layout
(each text + one 0x1E) is part of the recorded provenance of those
experiments: changing a single byte of the file destroys position
correspondence with every result already registered. V2 does not ask it to
change anything: V1 keeps its byte-0x1E framing and its existing XSEP/XPM1
sidecars, and V2 simply describes what future corpora do differently. The two
formats coexist; V1's readers and provenance are frozen, V2 is additive.
(For indexing pile.txt's byte stream, V1's own accounting already exists; a
V2 view of the same bytes is possible without touching the file — write the
tables, keep `pile.txt` as the source-access file — but that is a decision
for the owners of the registered experiments, not this spec.)

## 9. Tool surface (this lane)

```
xsa corpus --v2-meta        --shards DIR --out PREFIX [--limit-bytes N]
                            [--order FILE | --order-seed S]
xsa corpus --v2-materialize --prefix P [--shards DIR] [--jobs J]
xsa corpus --v2-reorder     --from P1 --out P2 (--order FILE | --order-seed S)
xsa corpus --v2-verify      --prefix P [--shards DIR] [--samples N] [--reference P1]
                            [--jobs J]
xsa corpus --v2-locate      --prefix P --offset O[,O...]
xsa build --tape --input <name>.bin --docs-map <prefix> ...   (§6)
xsa query <container> <pattern> --docs <prefix-dir>            (§6, §7)
```
