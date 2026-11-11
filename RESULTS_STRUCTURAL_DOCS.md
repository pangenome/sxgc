# RESULTS — structural document boundaries (feat/structural-docs)

**The requirement:** `NOTE_FROM_EMENDER_DOCUMENT_BOUNDARIES.md` — document
boundaries are *metadata*, never inferred from content bytes. D1: consult cuts a
position's spine by an explicit document base (resolved from the document
structure when the window omits it; refused when neither exists; never a scan).
D2: the document-start structure goes into the container as member m15 (SXI2 v10,
delta-varint + sampled absolutes, rank/select), embedded by
`xsa build --docs-map DIR`, byte-identical without the flag. D3:
`--exclude-source` removes the position's own occurrence before any count, and
the reply states it.

**Branch:** `feat/structural-docs` (merged to main). All gates below were run on
this box (2×AMD EPYC 7713, 256 threads, 1007 GiB RAM) on 2026-11-11, with foreign
load throughout (`load average` 40–99 while the consult arms ran: four
FastGA/FastGCD jobs at ~15 cores each plus GPU training jobs — the walls below
are honest walls, not quiet-box walls).

**Containers:** the banked `/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2` (63.16
GB, SXI2 v6, n = 100000000001) and its m15-embedded copy
(`pile-head100-v10.sxi2`, SXI2 v10 via `xsa docstarts embed --out`, the v6 copy
route); the 150 MB prefix container `pile150.sxi2` built by BOTH the pre-lane
binary (adadf48) and the lane binary. **The authoritative map:**
`/mnt/nvme2n1/erikg/pilemap_full` (210,607,728 documents, 1,307,910,802,540
bytes, seps.bin XSEP v1, verified to the byte against the shard records —
RESULTS_PILE_DOCMAP.md).

---

## G1 — boundary parity (rank/select vs seps.bin), zero disagreements

`gates/structdocs_g1.sh` runs two instruments: `xsa docstarts verify` (member
rank/select vs an independent anchor-bisect + delta-scan decoder of seps.bin) and
`xsa docstarts probe` diffed byte-for-byte against `gates/docstarts_oracle.c`
(a third decoder, plain C). Positions inside documents that contain an internal
0x1E are deliberately included — a 0x1E scanner would mis-cut every one of them.

**100 GB spine container** (16,119,767 embedded documents; the 268 documents
holding the 6,979 internal separator bytes of the first 100 GB all contribute a
mid-document position):

```
DOCSTARTS_PARITY positions=10268 internal_1e_positions=268 select_checks=10000 disagreements=0 disagreements_tol=0 method="member rank/select (delta-varint+sampled-absolutes) vs seps.bin XSEP-v1 anchor-bisect+delta-scan; both cross-checked against gates/docstarts_oracle.c" range=[0,100000000000) text_len=100000000000 docs_member=16119767 docs_map=210607728
DOCSTARTS_PARITY result=PASS
G1_PARITY member_vs_map disagreements=0 result=PASS
G1_ORACLE_POSITIONS random=10000 internal_0x1e_docs=268 total=10268 text_len=100000000000
G1_ORACLE_DIFF member_vs_c_oracle identical=1 lines=10268 result=PASS
G1_RESULT PASS
```

**150 MB prefix container** (25,454 documents; the first 150 MB contains 0
internal-0x1E documents — verified independently against meta.bin, the internal
separators concentrate deeper in the pile):

```
DOCSTARTS_PARITY positions=2000 internal_1e_positions=0 select_checks=2000 disagreements=0 disagreements_tol=0 ... range=[0,150000000) text_len=150000000 docs_member=25454 docs_map=210607728
DOCSTARTS_PARITY result=PASS
G1_ORACLE_DIFF member_vs_c_oracle identical=1 lines=2000 result=PASS
G1_RESULT PASS
```

**Member cost, measured:** 36,165,080 B over the 100e9-byte text = **0.00036 n**
(2.25 B/gap vs 8 B/doc for a plain u64 array, gaps_vs_plain=0.257); embed wall
171 s for the v6 copy route (63 GB streamed), 0.6 s in-place at 150 MB.

## G2 — no cross-boundary matches: 3 adversarial cases, bytes + before/after

`gates/structdocs_fixtures.py` builds three hand-crafted corpora (~50 MB each;
boundaries placed mid-pattern, never at a byte the content suggests) where every
histogram is compared against a brute-force scan (memmem, no index, no shared
code). The BEFORE arm is the pre-lane binary (adadf48, the 0x1E-cut build) run on
the same container text — its rows match its own removed rule exactly, which is
what makes the before/after difference real rather than asserted.

**Case g2a — internal separator** (doc A contains an internal 0x1E; the query
sits 7 bytes past it; the spine crosses it):

```
G2_BYTES g2a spine_tail=b'nalpharunalpharunalpharunalpharunalpharun\x1ebetaru' own_byte=110 first_byte_past_boundary=b'U'
G2_HISTOGRAM g2a arm=BEFORE_old_0x1e_cut_build cut_len=6 width=2000 eof=0 pairs=[(110, 2000)]
G2_HISTOGRAM g2a arm=UNRESTRICTED width=1 eof=0 pairs=[(110, 1)]
G2_HISTOGRAM g2a arm=DOC_RESTRICTED width=1 eof=0 pairs=[(110, 1)]
G2_HISTOGRAM g2a arm=RESTRICTED_EXCLUDED width=0 eof=0 pairs=[]
G2_BEFORE g2a old_binary ctxlen=6 width=2000 eof=0 pairs=[(110, 2000)] parity=exact
G2_WHOLEDOC g2a resolved_windows=3 rows=78034 result=exact
```

The old rule mangled the spine to its last 6 bytes (`betaru`, 2,000 cross-region
occurrences counted); the new rule keeps the whole same-document spine
(width 1, exact).

**Case g2b — occurrences on both sides of the boundary** (docs D and E end with
the SAME 300-byte block; the 256-byte spine occurs once in D and once in E):

```
G2_BYTES g2b spine_tail=b'UUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUUU' own_byte=68 first_byte_past_boundary=b'U'
G2_HISTOGRAM g2b arm=BEFORE_old_0x1e_cut_build cut_len=256 width=90 eof=0 pairs=[(85, 88), (68, 1), (69, 1)]
G2_HISTOGRAM g2b arm=UNRESTRICTED width=90 eof=0 pairs=[(85, 88), (68, 1), (69, 1)]
G2_HISTOGRAM g2b arm=DOC_RESTRICTED width=45 eof=0 pairs=[(85, 44), (68, 1)]
G2_HISTOGRAM g2b arm=RESTRICTED_EXCLUDED width=44 eof=0 pairs=[(85, 44)]
G2_BEFORE g2b old_binary ctxlen=256 width=90 eof=0 pairs=[(85, 88), (68, 1), (69, 1)] parity=exact
G2_WHOLEDOC g2b resolved_windows=2 rows=78658 result=exact
```

`--doc-restrict` excludes E's side exactly (90 → 45; E's continuation byte 69
leaves the histogram).

**Case g2c — an occurrence that SPANS the boundary** (the stream around the F/G
boundary is byte-identical to the spine occurring inside doc H; no separator
byte is involved anywhere — boundaries are metadata, not content):

```
G2_BYTES g2c spine_tail=b'SPANTESTssssssssssssssssssssssssssssssssssssssss' own_byte=97 first_byte_past_boundary=b't'
G2_HISTOGRAM g2c arm=BEFORE_old_0x1e_cut_build cut_len=256 width=2 eof=0 pairs=[(97, 1), (103, 1)]
G2_HISTOGRAM g2c arm=UNRESTRICTED width=2 eof=0 pairs=[(97, 1), (103, 1)]
G2_HISTOGRAM g2c arm=DOC_RESTRICTED width=1 eof=0 pairs=[(97, 1)]
G2_HISTOGRAM g2c arm=RESTRICTED_EXCLUDED width=0 eof=0 pairs=[]
G2_BEFORE g2c old_binary ctxlen=256 width=2 eof=0 pairs=[(97, 1), (103, 1)] parity=exact
G2_WHOLEDOC g2c resolved_windows=3 rows=56128 result=exact
```

The stream match that spans F/G (continuation byte `g` = 103) is dropped by the
restriction.

## G3 — source exclusion: width−1 on / width off, both directions, 120 positions

Same fixture gate; every position's `--exclude-source` width equals the raw
width minus one, the own byte's pair differs by exactly one, every other pair is
unchanged, and the row's flag bit 2 states the removal:

```
G3_EXCLUSION positions=120 width_on_equals_width_off_minus_1=120 both_directions=1 result=PASS
G2_G3_RESULT PASS
```

At 100 GB scale the A1 gate's `--exclude-source` arm adds 35 more positions
(billions of counts) with the same property (`excl_width=width−1` on every row,
see A1 below).

## G4 — determinism: identical rows across restarts and concurrent arms

150 MB container with embedded m15; 869 windows (3,476 positions) with bases
**resolved from the member** (no `base` in the windows), all under
`--exclude-source --doc-restrict`; three separate process runs, two at `-j 8`
and one at `-j 1`:

```
576d7ea8b515437150a5bf0e0881f9de3c2e4622dcab1f9fa3f33c8b2114eafb  g4-r1.rows
576d7ea8b515437150a5bf0e0881f9de3c2e4622dcab1f9fa3f33c8b2114eafb  g4-r2.rows
576d7ea8b515437150a5bf0e0881f9de3c2e4622dcab1f9fa3f33c8b2114eafb  g4-j1.rows
CONSULT_DONE positions=3476 windows=869 ... rows_sha256=576d7ea8b515437150a5bf0e0881f9de3c2e4622dcab1f9fa3f33c8b2114eafb ... excluded_rows=3476 restricted_rows=3476
```

The same-position run at 100 GB (the A2/A3/A4 harness) adds the restart/concurrent
arm for the plain path, and the A4 kill+resume arm re-derives the same bytes
after a crash. `xsa docstarts embed` is deterministic too: the same map embedded
into two copies of a container gives identical member bytes (the 150 MB
container's m15 is byte-identical whether embedded in-build or post-hoc).

## G5 — no regression

**Byte-identity of the untouched build:** the same 150 MB input built by the
pre-lane binary (adadf48) and by this lane's binary, both `--tape --no-trie`,
identical container bytes:

```
871c904f3a67856943a652d1bad743b826cf6892e2265852d9b4f619ff1f98d5  p150-old/pile150.sxi2   (adadf48 binary)
871c904f3a67856943a652d1bad743b826cf6892e2265852d9b4f619ff1f98d5  p150-new/pile150.sxi2   (this lane)
```

**The `--docs-map` arm** (same input, `xsa build --tape --no-trie --docs-map
/mnt/nvme2n1/erikg/pilemap_full`) embeds m15 in-build; the journal line:

```
DOCSTARTS docs=25454 bytes=57128 ratio_vs_n=0.000380853 encoding=delta-varint sample_stride=64 gaps_bytes=52300 plain_u64_bytes=203632 gaps_vs_plain=0.2568 text_len=150000000 map_corpus_bytes=1307910802540 in_place=1 append_s=0.6
```

**A1 — brute-force oracle at 100 GB** (the banked container, raw-spine arm +
`--exclude-source` arm; 10.6 billion counts):

```
A1_PARITY contexts=35 rows_checked=70 exact=35 mismatched=0 counts_compared=10611404039 exclude_rows=35 result=PASS
```

**A2/A3/A4 — the consult harness** (`gates/consult_a234.sh`, banked container,
96 jobs, load 40–99 throughout). The spine is now cut at the document base
instead of the last `0x1E` and the row layout is v2, so the row shas differ
from CONSULT.md's pre-lane receipt **by design**; the gate's own comparisons are
unchanged and all pass:

```
A3_RUN1 positions=1001472 positions_per_s=4556.521 peak_rss_gb=7.998 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_RUN2 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_TWO_RUNS identical=1 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_CAP rss2=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f rss8=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f rss16=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f
A2_WINDOW_FORMAT jsonl=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f bin=9695eb0175262afbae9c3a23dc3564385b690d4373d1f5309700bdebe6b3572f
A2_FULL_1M rss8=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0 rss16=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A2_DETERMINISM two_runs=1 kill_resume=1 rss_cap_2v8v16=1 window_format=1 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A3_BOUNDED positions=1001472 positions_per_s=4556.521 peak_rss_gb=7.998 cap_gb=8 jobs=96
A4_KILL exit=124 (124 = the harness's SIGTERM, the crash this gate needs)
A4_KILL file_bytes=35854379 last_commit_output_bytes=30626268 commits=2
A4_RESUME positions=1001472 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0 truncated_bytes=5228111
A4_RESUME_IDENTICAL to_uninterrupted_run=1 positions=1001472 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
A4_CRASH_RESUME positions=1001472 sha256=6058073fa14e0230ecf962052369348b041512b0c7e45ba32a25e29ded299bf0
```

**cargo clean + tests:** `cargo clean` then a full `cargo build --release` then
`cargo test --release`: **90 passed, 0 failed** (1 ignored, the pre-existing
selftest sweep marker).

## G6 — merge to main (origin/main moved mid-lane: the --docs-map collision)

While this lane ran, origin/main advanced (adadf48 → `c05bea2`, the corpus-v2
lane's four commits) — and that lane ALSO wired a `--docs-map` flag into
`xsa build`, with different semantics: their `--docs-map PREFIX` validates a
CORPUS V2 sidecar pair (`PREFIX.starts` + `PREFIX.docs`) and journals
`CONTAINER_DOCS_V2`; this lane's `--docs-map DIR` embeds m15. Both contracts
are published, so the merge keeps BOTH behind one flag, **dispatched at parse
time by the argument's shape** (a directory holding `seps.bin` → the m15 embed;
a prefix holding the pair → the V2 validation; anything else → a hard error
naming both forms — never guessed). Post-rebase verification, all with the
final binary:

```
--docs-map /nonexistent     -> "neither a document-map directory (want DIR/seps.bin ...) nor a CORPUS V2 sidecar pair (want PREFIX.starts and PREFIX.docs ...)"
--docs-map v2test (dummy pair) -> routed to corpusv2::docs_map_check: "./v2test.starts is not an XCS2 v1 starts table"
150 MB, no flags            -> 871c904f3a67856943a652d1bad743b826cf6892e2265852d9b4f619ff1f98d5 (byte-identical to the adadf48 build)
150 MB, --docs-map pilemap  -> dd56e422811c55374ded1a4c703aeff0219427be86a90e55bb371f0ecec99faa (byte-identical to the pre-rebase embed)
G1 verify on that container -> DOCSTARTS_PARITY ... disagreements=0 ... result=PASS
G4 window set, all flags   -> 576d7ea8b515437150a5bf0e0881f9de3c2e4622dcab1f9fa3f33c8b2114eafb (identical to the pre-rebase runs)
cargo test on the merged tree -> 98 passed, 0 failed (90 this lane + 8 corpus-v2)
```

The lane is rebased linearly onto `c05bea2` (no merge commit; the repo's
history is linear) and main fast-forwards to it.
