# FORWARD-ONLY: one container format, one reader, no degraded modes

Branch `feat/forward-only` (from `origin/main` @ 5b5bb0c). Operator directive:

> "why do we serve the old format? we're the only consumers. we should go forward."

**What this change does.** The container family has exactly ONE supported shape —
what `xsa build` emits today: **SXI2 v10**, i.e. the block-compressed run stream
(m1 codec 116 with its m12/m13 directories), the locate anchors (m11), χ (m5), the
document starts (**m15**) and the context trie (**m20**). Every command refuses any
other file **by name**, with the rebuild instruction, and there is no second
reader, no degraded mode and no `--legacy-*` escape hatch:

```
$ xsa query /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 --count "the "
xsa: error: /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2: SXI2 v6 container (an older shape: below the supported one)
  this build serves exactly ONE container format: SXI2 v10 with m15 (document starts) and m20 (context trie)
  rebuild with `xsa build`.
```

> **Artifacts in old formats are UNSUPPORTED.** `.ri4`, SXI1 and SXI2 v2..v9 are
> not readable, not convertible in place and not repaired by any flag: **they must
> be rebuilt from the corpus with `xsa build --docs-map …`**. The banked SHAs in
> git remain historical records of what was measured; they do not imply — and this
> change deliberately removes — the ability to read the files they refer to. The
> family may still gain members over time; that is additive and needs no reader
> change.

## What was removed, and why

### 1. The phi-era readers and everything that called them

| file | lines | what it was |
|---|---|---|
| `xsa/src/product.rs` | 1152 | the Ri4 engine: `query-legacy`, `mems`, `serve`, `witness-build` |
| `xsa/src/forks.rs` | 901 | the χ fork matrix (reads the m2/m3 phi edges) |
| `xsa/src/witness.rs` | 143 | the witness index builder (m8) |
| `xsa/src/sxi2.rs` | 570 → 63 | `runs()` (sequential Huffman+gamma codec), `Phi` (m2/m3 edges), `LfMap` (the LF starts map), `LiteAnchors` (1/1024 anchors), the packed head/tail samples. Only `chi()` (the m5 export) remains |
| `xsa/src/main.rs` | −1246 / +58 | `Ri4`, `Bytes`, `PackedSa`, `Anchors`, `read_raw_runs`, `search_sub`, `ms_vector`, `cmd_query`, `cmd_build_anchors`, `cmd_chi_rspace`, the `.ri4` header parser, `stats --rlbwt` |

None of them could read the one shape: `xsa mems` on the current 100 GB
container already died today with `SXI2: run member size` (m1 codec 116), and
`query-legacy` was already refusing the current container by name. Keeping the
names would have been a surface advertising a format this build no longer
serves, so they are deleted and the retired names refuse by name:

```
xsa mems: this subcommand was REMOVED with the phi-era readers (.ri4, SXI1,
  SXI2 v2..v9). This build serves exactly ONE container format: SXI2 v10 with
  m15 (document starts) and m20 (context trie)
  rebuild with `xsa build`; `xsa query` / `search` / `consult` / `seeds` / `mem`
  / `branches` / `lcp` read it.
```

**A correction to the task's premise.** The task listed `xsa/src/qlite.rs` as
"the SXI2 v6-lite reader" to be deleted. It is the opposite: `qlite.rs` is the
*current* query kernel — the module the whole read surface (`query`, `search`,
`consult`, `seeds`, `fanout`, `mem`, `branches`, `stateless-bench`, and the
`DocStarts`/`Ctx`/`LcpRuns` accessors) is built on, and the thing that makes
`xsa query --count` cost milliseconds on a 63 GB container. Deleting it would have
deleted the product and failed gate F2 outright. What was legacy *inside* it was
the version span and the fallbacks, and that is what was removed:

* `Reader::open` used to accept `SXI2 v6..=v10`, with a v6 branch for the
  pre-incremental member directory at offset 64. It now accepts **v10 only**,
  with one layout (the trailing directory), and requires the members
  `m1,m10,m11,m12,m13,m15` with `m15` declared by flag bit 512, and m1 codec 116.
* `describe_format` no longer says "v6 lite" for anything: it names what the file
  *is* (`SXI2 v6 container (an older shape: below the supported one)`) so the
  refusal is legible, and nothing else.
* The refusal is ONE function, `qlite::refuse(path, what)`, and one constant,
  `qlite::SUPPORTED_SHAPE`. `sxi::Container::open` (the header/validator reader
  behind `stats` and `sxi-info`) calls the same two, so every command refuses in
  the same words.
* `sxi.rs` lost its version matrix (`v1..v10`: the pre-lite member-id inference,
  the pre-v7 codec table, the SXI1 body), its whole-file member CRC sweep's
  phi-era decoders, and the `SXI1`/v1..v9 acceptance.

`m20` is required by the commands that READ it, not by the gate: `xsa build
--no-trie` is an explicit product arm, and its container must still answer
`query`/`search`/`consult`/`seeds`/`mem`/`branches`. Those commands refuse with
`qlite::trie_required` — `this container carries no member m20 (the context trie;
this command reads it and has no other arm)` + the shape + the rebuild line —
instead of silently dropping to the zero-trie mode. Gate F6 proves both halves.

### 2. The `--legacy-merge` build route

The pre-bin front end (libsais chunk front end → `cross_lcp_merge --tree` k-way
walk → adopt finish) is gone from `chain.rs`, with `plan::model` (its pricing)
and `plan::model_fanin` (the k-way fan-in pricing). Its flags are refused **by
name**, so a script learns why instead of being told "unknown option" and left to
guess:

```
--legacy-merge is REMOVED: the pre-bin chunk front end + k-way merge walk no
  longer exists. The bin pipeline (the default) and --tape are the only routes;
  drop --legacy-merge
--chunks belonged to the chunk front end + k-way merge walk, which this build
  does not have: that route is REMOVED. `xsa build` is the bin/tape pipeline
  (the producer sweeps the corpus itself); drop --chunks        [same: --chunk-mb, --kway, --fanin]
```

The bin route and the tape route stay (`--tape` selects the producer; it is not a
second route), and `--v2-pipeline` is still accepted-and-journaled-as-ignored.
`Options` lost `legacy_merge`, `chunks`, `kway`, `chunk_mb`, `fanin` and
`bin_pipeline` is now a constant of the one route.

### 3. Every "member absent → degrade" branch

`DocStarts::from_container` returns a `DocStarts` — not an `Option` — because m15
is part of the shape. The compiler then found every fallback for us; the branches
are deleted, not re-worded:

| file | what the fallback did | now |
|---|---|---|
| `consult.inc.rs` | `boundary=absent`: every row marked `boundary_exact=0` (the conflated histogram) | no such arm; `--docs DIR` stays an **explicit override** that is cross-checked against the member (sidecar vs member disagreeing is a hard error) |
| `consult.inc.rs` | ran the zero-trie mode when m20 was missing | `qlite::trie_required` refusal |
| `branches.inc.rs` | refused the default and offered `--docs DIR`/`xsa docstarts embed` as fixes | structure comes from the member; `--cross-doc` stays a **semantic** opt-in (not a format fallback) |
| `search.inc.rs` | "the crossing-inclusive (legacy) list is the only answer that exists here" | no such arm; `--cross-doc` still opts in semantically |
| `seeds.inc.rs` | `doc_absent` counter, `-` doc field per row, "a docid is never guessed" | every row's doc is exact; the counter is gone |
| `ctx.rs` | `ctx-build` and `ctx-verify` arms for "no m15"/"no m20" | `ctx-build` is REMOVED (its input was a container *without* m20, which can no longer exist); `ctx-verify` reads the member |
| `docstarts.rs` | `info` printed `present=false reason=no-member-m15`; `embed` appended to a container without m15; a v6 copy route (`append_copy`) | `info` reports the member (the `present=true` field stays, as a constant); `docstarts embed` is REMOVED as a command; `append_copy` deleted |
| `fanout.inc.rs`, `mem.inc.rs`, `seeds.inc.rs` | a longer "supported: SXI2 v6 lite … migration: …" epitaph after the open error | the open error *is* the refusal; nothing appended |

`--cross-doc` is kept everywhere it is a **semantic** opt-in (the conflated
crossing-inclusive answer) — it never was and is not now a format fallback.
`--docs DIR` (a superset XSEP map) likewise stays as an explicit override, never
as a repair for a structure-less container.

### 4. Help text and docs

`main.rs`'s `USAGE` lost the `mems`/`forks`/`witness-build`/`serve`/
`query-legacy`/`stats --rlbwt` lines and now ends with a THE ONE CONTAINER
FORMAT block; README gained a "One container format, and nothing else" section
with the verbatim refusal; REPRODUCE.md's refusal table quotes the real messages
(shape + rebuild, removed flags, required `--docs-map`); SERVING.md lost its
v6-lite and "mems is superseded" claims; `bit6/SXI_FORMAT.md` opens with a
FORWARD-ONLY banner declaring every layout below it historical, and its reader
compatibility section now describes the one shape; SPEC_CHAIN_TO_PILE.md lost the
third route.

## Two consequences of the policy you should know about

1. **`xsa build` now REQUIRES `--docs-map`.** A container without m15 is not
   queryable by any command, so a build without a map would spend the run
   producing an artifact nobody can open. The check is at the top of
   `run_chain`, before any plan or phase:
   `--docs-map is REQUIRED: the one container format carries m15 (document
   starts), and a container without it is not queryable by any xsa command …`
2. **Both `--docs-map` shapes now embed m15.** `--docs-map DIR` (the XSEP pile
   map) always did; the CORPUS V2 arm (`--docs-map PREFIX`) did not, and would
   have produced a v9 container. It now streams `PREFIX.starts` through a new
   `Source::CorpusV2` and embeds the member like the DIR arm.
3. **The joins run through a builder door, and the result is certified.** The
   finish stage's container and the m20/m15 joins are half-built files (v7..v9,
   no m15) which the query gate rightly refuses. `qlite::open_partial` is the ONE
   other entry point: header + trailing directory, no mmap of the run stream, no
   block cache, no kernel — a query cannot be built on it. After the last join the
   driver reopens the published container **through the query gate** and journals
   `CONTAINER_CERTIFIED format=sxi2-v10 version=10 … m15=… m20=…`; a build that
   did not land the one shape cannot exit 0.

## Gates (verbatim, numbers)

Box: 256 cores, load average 6..72 during the runs (other lanes were active —
noted because the F2 walls below are shared). Every driver script wraps its work
in `setsid timeout --kill-after=10 N`; no survivor processes were left behind.

### F1 — BUILD + TESTS

```
$ cargo build --release
    Finished `release` profile [optimized] target(s) in 0.06s      # clean, warnings only
$ cargo test --release
running 99 tests
test result: ok. 98 passed; 0 failed; 1 ignored; 0 measured; 0 filtered out; finished in 2.19s
```

Baseline measured the same way on `origin/main` (5b5bb0c, separate worktree):
`test result: ok. 103 passed; 0 failed; 1 ignored`. The five that are gone were
tests *of the deleted readers and routes*: 2 in `main.rs`'s `run_width_tests`
(the Ri4 run-width reader and the SXI1 header round trip), `chain::
legacy_merge_is_the_only_route_that_takes_the_chunk_knobs`, `chain::
phrase_radix_is_refused_on_the_legacy_route`, and `recovery::
legacy_route_cannot_install_tape_recovery` (rewritten as `the_removed_merge_
route_cannot_install_tape_recovery`). The 1 ignored test is the pre-existing
`stateless_tests::pile_counts_histograms_and_retirement` (needs the 100 GB
container).

### F2 — THE CURRENT CONTAINER STILL WORKS (byte identity, pre vs post binary)

`gates/forward_only_f2.sh BINARY CONTAINER CORPUS OUTDIR [JOBS]` runs 8 commands
and sha256-manifests every output; the same script ran against `/tmp/pre-xsa`
(built from `origin/main`) and against this branch's binary.

Container `/mnt/nvme2n1/erikg/PILE100-REHEARSAL/pile-head100.sxi2` (SXI2 v10,
65,533,578,480 B), corpus `/mnt/nvme2n1/erikg/pile-head100.txt` (100,000,000,000 B),
`-j 32`:

| command | output | sha256 (pre == post) |
|---|---|---|
| `query --count "the "` | `0  the  704942163` (**704,942,163** ✔) | `1d9caf4b…f8239da` |
| `search "Harper Reed"` | 55 rows (55 occurrences, **verified** against the corpus bytes) | `818196a6…49352a7` |
| `search --jsonl` | 55 objects | `926dc2d5…584fe20` |
| `consult` 200 windows (doc/base from m15, `start=1024 len=16 left=1024`, corpus-checked) | 200 rows (binary `XSACONS1`) | `7ad04dd1…d797af3` |
| `branch-of "the "` | 160 children, `count=704942163` | `c397ab77…004ab91` |
| `seeds` on a 200-byte corpus query | 47 rows | `2c6bcd06…b797e2` |
| `docstarts probe` 200 positions | 200 rank/select rows | `abd2eaa3…691b5b9` |
| `docstarts info` | 1 row, `present=true docs=16119767` | `d945c3a3…16ef7a2` |

```
$ diff /tmp/f2-pre/MANIFEST.sha256 /tmp/f2-post/MANIFEST.sha256 && echo IDENTICAL
F2 IDENTICAL: 8/8 files byte-identical
```

8 commands, 8 outputs, all byte-identical, and the task's six probe classes are
all covered (query --count, search, consult, branch-of, seeds, docstarts — the
last with both `probe` and `info`). The counts themselves are the pre-existing
verified numbers: **704,942,163** for `"the "` and **55** for `"Harper Reed"`.

### F3 — THE OLD CONTAINER IS REFUSED (7 commands)

`gates/forward_only_f3.sh BINARY OLD_CONTAINER CORPUS OUTDIR` against
`/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2` (63,163,489,696 B, magic
`5358493206000000` = SXI2 **v6**). Each message is checked for three properties:
the format is named, `rebuild with \`xsa build\`` is present, and no legacy
escape hatch is mentioned.

```
F3_PASS name=query      F3_PASS name=consult    F3_PASS name=seeds   F3_PASS name=search
F3_PASS name=branches   F3_PASS name=mem        F3_PASS name=stats
F3_COMMANDS 7
F3_DONE fails=0
```

The message, verbatim (identical for all 7; `search` prefixes its own name):

```
xsa: error: /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2: SXI2 v6 container (an older shape: below the supported one)
  this build serves exactly ONE container format: SXI2 v10 with m15 (document starts) and m20 (context trie)
  rebuild with `xsa build`.
```

### F4 — NO DANGLING SURFACE

Counts over `*.rs` + `*.md` (excluding build artifacts and this file). "crate"
is `xsa/src` only.

| token | origin/main (all) | now (all) | origin/main (crate) | now (crate) |
|---|---|---|---|---|
| `qlite` | 90 | 94 | 59 | 64 |
| `query-legacy` | 7 | 2 | 6 | 2 |
| `legacy-merge` | 32 | 15 | 26 | 10 |
| `phi-era` | 8 | 10 | 6 | 6 |
| `v6 lite` | 33 | 6 | 20 | **0** |
| `is not a queryable index` | 3 | **0** | 2 | **0** |

Every remaining hit is either the *live* module name or an explicit historical
note, and each class is accounted for:

* `qlite` **went up** on purpose: the name is the CURRENT kernel (`qlite::refuse`,
  `qlite::SUPPORTED_SHAPE`, `qlite::open_partial`, `crate::qlite::Reader`). No
  reference to a qlite-era *command* survives; `query-legacy` is down to 2 hits,
  both in the retired-name refusal arm of `main.rs`.
* `legacy-merge` (15): 10 in the crate = the refusal message + its tests; 2 in
  README/REPRODUCE = the removal notice; 3 in `tools/binbwt/RESULTS_AUTO_RADIX.md`
  and `RESULTS_PHRASE_RADIX.md` = **historical measurement reports, untouched**
  (that directory is another lane's; see "Residual risks").
* `phi-era` (10): every hit is a comment or message that says the phi-era readers
  are RETIRED/GONE (`sxi.rs:303`, `qlite.rs:11,3101`, `main.rs:13,391,398`,
  `SERVING.md:153`, `SXI_FORMAT.md:244`, `REPRODUCE.md:1171`) — i.e. explained as
  historical, which is the gate's alternative to deletion.
* `v6 lite` (6): 1 in `RESUME_DESIGN.md` (a historical design note about which
  writer it targeted), 5 across `tools/binbwt/RESULTS_QUERY_{QLITE,HOTPATH,
  PROFILE}.md` — historical reports, untouched for the same reason. **Zero in the
  crate.**

### F5 — SIZE

```
origin/main : 35,626 lines in 25 files  (xsa/src/*.rs)
now         : 30,929 lines in 22 files  (xsa/src/*.rs)
delta       : -4,697 lines (-13.2%), -3 files
git diff --stat origin/main -- xsa/src : 18 files changed, 762 insertions(+), 5459 deletions(-)
deleted files: xsa/src/product.rs (1152), xsa/src/forks.rs (901), xsa/src/witness.rs (143)
```

Per-file net (insertions/deletions): `sxi2.rs` 11/518, `main.rs` 58/1246,
`ctx.rs` 29/303, `chain.rs` 150/294, `docstarts.rs` 120/264, `sxi.rs` 50/161,
`consult.inc.rs` 26/42, `branches.inc.rs` 14/37, `seeds.inc.rs` 24/45,
`search.inc.rs` 6/17, `recovery.rs` 5/4, `qlite.rs` 260/99 (the gate, the builder
door and their doc comments are new; the version span and every fallback are
gone), `plan.rs` (model/model_fanin removed).

### F6 — THE BUILD STILL EMITS THE ONE SHAPE, AND IT READS BACK

`gates/forward_only_f6_build.sh BINARY WORKDIR [BYTES] [DOCS]` — a 64 MB fixture
corpus with 4,096 documents and an **independent** XSEP v1 map written by
`gates/forward_only_fixture.c` (no index code linked), through
`xsa build --tape --docs-map`:

```
F6_BUILD rc=0
CONTAINER_CERTIFIED format=sxi2-v10 version=10 n=64004096 runs=63673072 members=m1,m4,m5,m7,m10,m11,m12,m13,m15,m20 m15=9008 m20=31804320
CONTEXT_TRIE_JOIN member=m20 codec=20 bytes=31804320 container_bytes=396900272 -> 428704632
DOCSTARTS_JOIN member=m15 present=true already=0 docs=4096 bytes=9008 encoding=delta-varint
F6_PASS query | mem | branch_of | trie_info | docstarts | stats | consult | seeds
F6_COUNT want=4096 got=4096
F6_PASS name=build-without-docs-map rc=1
F6_PASS name=build-no-trie rc=0
F6_PASS name=no-trie-query answers
F6_PASS name=no-trie-branch-of refuses by name rc=1
F6_CHECKS n=13 ok=13
```

The `--no-trie` refusal, verbatim:

```
xsa: error: /tmp/f6/no-trie/out.sxi2: this container carries no member m20 (the context trie; this command reads it and has no other arm)
  this build serves exactly ONE container format: SXI2 v10 with m15 (document starts) and m20 (context trie)
  rebuild with `xsa build`.
```

## Residual risks

1. **`tools/binbwt/*` was not touched** (another lane owns it), so its historical
   reports still describe `--legacy-merge`, `mems`, `query-legacy` and "v6 lite"
   containers in the present tense. They are measurement records, not
   instructions; the two files that read as *instructions* going forward
   (`RESUME_DESIGN.md`'s v6-writer note) are historical design notes and were left
   as such. If the operator wants the historical ledger re-labelled, that is a
   separate mechanical pass.
2. **Old gate scripts in `gates/` now fail by design** wherever they
   build without `--docs-map` (`structdocs_fixtures.py`,
   `trie_in_build_g1.sh`, `no_cross_x*.sh` build arms) or call the removed
   `ctx-build`/`docstarts embed`/`mems`/`serve`. They are the historical gates of
   the shapes that are gone; F2/F3/F6 are the gates of the one shape. I did not
   rewrite them, because rewriting a gate to match a deletion is how a gate stops
   being a witness — the honest move is that they now fail loudly.
3. **`--no-trie` containers are legal but partially served.** They answer
   query/search/consult/seeds/mem/branches and REFUSE the trie-reading commands
   (branch-of, trie-stat, trie-info, relax, ctx-verify, ctx-curve, consult's trie
   layer). That is a deliberate split: m15 is the shape's boundary structure, m20
   is a member a command either needs or does not. If the operator wants m20 in
   the open gate too, it is one line in `Reader::open` — but then `--no-trie`
   becomes an unqueryable build arm and should be removed with it.
4. **`xsa build` now requires `--docs-map`.** A corpus with no document structure
   at all can no longer be built into a queryable container. That follows from
   "m15 is part of the shape", but it is a product restriction, not a refactor:
   say so if it is the wrong call.
5. **Format the reader no longer validates deeply.** `sxi::Container::open` (used
   by `stats`/`sxi-info`) used to decode the whole run stream, the phi edges and
   the LF starts map as a "validation". Those decoders are gone, so `stats` now
   checks the header, the directory, the member CRCs and the χ set — it no longer
   proves the run stream decodes. `qlite::Index::open` (the query kernel) still
   audits one block and both directories on every open, and `xsa verify
   --corpus` remains the byte certificate.
6. **F2's walls are not reported** as benchmarks: the box carried a load average
   of 6..72 from other lanes during the runs, so only the counters (which are
   exact) are quoted, not the seconds.

## Commits

```
a660adf forward-only: ONE container format, phi-era readers deleted
67eb563 forward-only: delete the --legacy-merge route and every member-absent fallback
d0eee52 forward-only: docs state the one format; gates F2/F3/F6
```
