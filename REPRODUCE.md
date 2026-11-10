# REPRODUCE — dump → queryable succinct index, from nothing

Everything below was run on **this** box on 2026-11-01 (commit `6dd06ef`, branch
`reproduce-guide`) except where a step is explicitly marked **NOT RE-RUN HERE**.
Where a command's output is quoted, it is the literal output of that command, not
a reconstruction. Box: 2×AMD EPYC 7713 (256 threads), 1007 GiB RAM,
504 GiB tmpfs on `/dev/shm`, Ubuntu 24.04.

---

## 1. WHAT THIS IS

1. `xsa` is a streaming builder for a **succinct full-text index over arbitrary
   bytes**: the tape route consumes raw bytes with a virtual END and publishes one
   self-contained container (`.sxi2`) that answers exact substring queries.
2. Three commands, in order: **`xsa corpus`** (dump → corpus bytes + document
   sidecars), **`xsa build`** (corpus → container), **`xsa query`** (container +
   patterns → offsets).
3. Memory is **bounded and flat in n**: the build's RAM is a fixed-size band arena
   chosen by `--memory-gb`, and positions live in bounded rolling files on scratch,
   never in a corpus-sized array.
4. **No suffix array is materialised** on any disc or in any heap: the build asserts
   `full_SA=0 persistent_positions=0` on every run, and the tape route deletes each
   band file as the finish folds it.
5. The index is a run-length BWT (r runs) plus anchors; exact matching is backward
   search plus one LF walk per occurrence.

---

## 2. REQUIREMENTS

Measured on the box above; the numbers that matter are cores, RAM and scratch.

| what | requirement | why |
|---|---|---|
| cores | any; 16 is fine, 96+ is what the large rungs used | `--threads` is both the sort parallelism and the number of **bands in flight**, so it divides the RAM arena (see below). |
| RAM | **must exceed `--memory-gb`**, with room for a second process | `--memory-gb` is a hard `RLIMIT_AS` applied to *every* phase. The producer (band sorter) and the finish run **concurrently**, each under that same cap, so a box with exactly `--memory-gb` of RAM will OOM. Keep 2× headroom. |
| RAM sizing rule | `--memory-gb` sets the **band arena** = half of it (`plan::tape_envelope_bytes` = `memory_gb/2` GiB); `rows_per_band = 0.85 × (envelope − fixed − staging) / (bytes_per_row × threads)` with `bytes_per_row = 90` (LCP slices on, the default) or `41` (`XSA_LCE_VERIFY=1`, the A/B arm). | The band size is **derived from `--memory-gb` and `--threads`, never from the corpus**. That is what makes the 100 GB rung possible at 96 GB. |
| scratch disk | the fit check prices the tape route at **1.00 n** (the measured `0.789 n` envelope at the Pile's first 100 GB x `factor=1.00`); observed: **2.19 n** at 150 MB and **2.46 n** at 10 GB (fixed overheads dominate at small n), then **~0.62 n** for the container | The declared window is 2 n of rows (`--budget-bytes`) plus pieces, freed as bands are consumed; the measured peak at 100 GB is `0.789 n` (`tools/binbwt/RESULTS_PILE_100GB.md`). The plan never gates on the other shape's tree, and `XSA_PLAN_DISC_MODEL` prints which model produced the number. |
| OS | native Linux x86_64, POSIX shell | `xsa/build.rs` refuses anything else: *"xsa bundled native stages currently require a native Linux build"* / *"…support Linux"* / *"…supports Linux x86_64"*. |
| rust | `cargo` + `rustc` (used: 1.97.1) | builds the driver and, through `build_tools.py`, the bundled native stages. |
| C/C++ | `g++` ≥ 10 (used: 13.3.0), `make`, `cmake`, `patch` | the bundled `xsa_finish` / `cross_lcp_merge` / `scatter_sort` / vendored pfp++ & AGC. |
| python3 | ≥ 3.12 (used: 3.12.3) | `xsa/build.rs` runs `python3 -I build_tools.py`; `xsa build` runs the bundled pipeline with `python3`. |
| bzip2 | `bzip2` **or** `pbzip2` on `PATH` | `xsa corpus` spawns `pbzip2 -dc` and falls back to `bzip2 -dc`. This box has **only** `bzip2 1.0.8` (no `pbzip2`), which is why the 100 GB transform took 3,883 s. |
| `/usr/bin/time` | GNU time | the journal records per-phase wall and peak RSS. |

---

## 3. BUILD THE BINARY

```sh
cd xsa && cargo build --release
```

Real output (first build in a fresh worktree; it compiles the bundled native
stages — AGC, pfp++, rpfbwt, libsais — so it is minutes, not seconds; abridged
below, the native sub-builds print a lot between these lines):

```
   Compiling sha2 v0.10.9
   Compiling rayon v1.12.0
   Compiling tempfile v3.27.0
warning: `xsa` (bin "xsa") generated 26 warnings (run `cargo fix --bin "xsa" -p xsa` to apply 5 suggestions)
    Finished `release` profile [optimized] target(s) in 4m 13s
```

The binary is `xsa/target/release/xsa` (52 MB; the native stages are embedded in it).
`BUILD_JOBS=N` limits native parallelism (capped at four).

**The bundle is compiled by the build script and hashed.** `xsa/build.rs` runs
`python3 -I build_tools.py`, which compiles the stages and emits a manifest; on
first `xsa build` the stages are extracted to
`${XDG_CACHE_HOME:-$HOME/.cache}/xsa/<manifest-sha256>/` and every file is checked
against `MANIFEST.sha256` there before launch. This run used:

```
/home/erikg/.cache/xsa/dd36f399a1cd71bd5341531ac72cfd2b20d8457c6d588a331325fa4ebafc7dcd/{scatter_sort,cross_lcp_merge,xsa_finish}
```

A mismatch is fatal, not a warning: `BUNDLE_TOOL_MISMATCH <name>: <path> sha256=<actual> expected=<expected>`.

**⚠ Editing `bit6/` or `tools/` does nothing until you repackage.** The crate
compiles `xsa/runtime/`, a *snapshot*; `python3 tools/package_xsa_sources.py`
mirrors repo `bit6/*.cpp`, `tools/binbwt/*.cpp` … into `xsa/runtime/`
(repo → snapshot, one-way). Verified in this checkout:

```sh
$ cmp bit6/xsa_finish.cpp xsa/runtime/bit6/xsa_finish.cpp && echo SAME
SAME
$ cmp tools/binbwt/scatter_sort.cpp xsa/runtime/tools/binbwt/scatter_sort.cpp
# differ: 87,303 vs 130,934 bytes — see §10, this is a real trap
```

So: edit → `python3 tools/package_xsa_sources.py` → `cargo build --release`.
There is **no** source digest file to refresh (it was deleted on 2026-10-30);
provenance is git's, journaled as the first line of every build log (§6).

---

## 4. GET THE DATA

The corpus of record is the English Wikipedia `pages-articles-multistream` dump.

```
https://dumps.wikimedia.org/enwiki/20261001/enwiki-20261001-pages-articles-multistream.xml.bz2
```

- size: **26,917,225,724 B = 26.9 GB** compressed (verified by `curl -sI` →
  `Content-Length: 26917225724`).
- published **sha1** (from the dump's own checksum list):
  `992b5ca87b3404894f013e59a43efbb8b1bc012b`
- published **md5**: `85ebcbc1f7a8b2503138c93b627150d5`
- the checksum lists are the **whole-directory** files
  `enwiki-20261001-sha1sums.txt` and `enwiki-20261001-md5sums.txt`
  (2,395 lines each; `/enwiki/20261001/` links both). Per-file `.sha1` siblings
  404. **There is NO sha256 list**: `enwiki-20261001-sha256sums.txt` returns
  `HTTP/1.1 404 Not Found` (verified). Do not invent one.

Fetch and verify (these are the commands that were run; the dump was already on
the box, so the download line is the only step **NOT RE-RUN HERE**):

```sh
cd /mnt/nvme2n1/erikg/wikidump
curl -O https://dumps.wikimedia.org/enwiki/20261001/enwiki-20261001-pages-articles-multistream.xml.bz2   # 26.9 GB
curl -s https://dumps.wikimedia.org/enwiki/20261001/enwiki-20261001-sha1sums.txt -o sha1sums.txt
grep 'pages-articles-multistream\.xml\.bz2$' sha1sums.txt
sha1sum enwiki-latest-pages-articles-multistream.xml.bz2
```

Real output:

```
992b5ca87b3404894f013e59a43efbb8b1bc012b  enwiki-20261001-pages-articles-multistream.xml.bz2
992b5ca87b3404894f013e59a43efbb8b1bc012b  /mnt/nvme2n1/erikg/wikidump/enwiki-latest-pages-articles-multistream.xml.bz2
```

The `enwiki-latest-…` snapshot that was downloaded on 2026-10-30 is byte-identical
to `20261001` (same 26,917,225,724 B, same sha1 → `sha1sum` agrees).

**Any dump of this shape works** — `xsa corpus` parses MediaWiki XML, not a
particular month. A tiny fixture is enough to exercise the step (§5).

---

## 5. TRANSFORM: dump → corpus + sidecars

Flag spellings, from the binary (`xsa corpus --help`):

```
xsa corpus --wikipedia <dump.xml.bz2|-> --out <dir> [--separator 0x1e|none]
```

Real invocation and output (this is a **synthetic 978-byte, 5-page fixture**
built here — a real multistream dump is the same command with a 26.9 GB argument):

```sh
cat > fixture.xml <<'XML'
<mediawiki xmlns="http://www.mediawiki.org/xml/export-0.11/" version="0.11" xml:lang="en">
  <siteinfo><sitename>Wikipedia</sitename><namespaces>
    <namespace key="0" case="first-letter">Main</namespace></namespaces></siteinfo>
  <page><title>Anarchism</title><ns>0</ns><revision>
    <text bytes="12" xml:space="preserve">Anarchism is a political philosophy.</text>
  </revision></page>
  <page><title>Béla Bartók</title><ns>0</ns><revision>
    <text bytes="9" xml:space="preserve">Bartók was a composer &amp; pianist.</text>
  </revision></page>
  <page><title>Category:Living people</title><ns>14</ns><revision>
    <text bytes="5" xml:space="preserve"></text>
  </revision></page>
  <page><title>Deleted text page</title><ns>0</ns><revision><text deleted="deleted" /></revision></page>
  <page><title>Tab</title><ns>0</ns><revision>
    <text bytes="30" xml:space="preserve">line one
line two	with a tab and trailing spaces   </text>
  </revision></page>
</mediawiki>
XML
sed 's/political philosophy/political\o036philosophy/' fixture.xml > fixture_0x1e.xml
split -n 3 -d fixture.xml part. && for f in part.0*; do bzip2 -c "$f" > "$f.bz2"; done
cat part.0*.bz2 > fixture.ms.bz2          # 3 concatenated bzip2 streams = the multistream shape
bzip2 -dc fixture.ms.bz2 | cmp - fixture.xml

xsa corpus --wikipedia fixture.ms.bz2 --out out
```

```
xsa corpus: docs=5 corpus_bytes=198 separators=5 separator=0x1e seps.bin=76 titles.bin=114 sha256(corpus.bin)=2ea31892a8e2a9c7c0efe950137c7aa5863920de00b260003ea49a17587089a3
xsa corpus: manifest out/MANIFEST.json
```

It emits exactly four files, and `MANIFEST.json` is written **last** (its presence
is the completion signal):

| file | content |
|---|---|
| `corpus.bin` | `"<title>\n<body bytes>\x1E"` per document — this **is** the corpus, nothing else |
| `seps.bin` | 48 B header (`XSEP`, version 1, flags, anchor_stride=64, doc_count, corpus_bytes, anchor_count, base) + `doc_count × u32` start deltas + every-64th `u64` absolute start |
| `titles.bin` | 20 B header (`XTIT`, version 1, doc_count, payload_bytes) + concatenated titles + `doc_count × u32` offsets + `doc_count × u16` lengths |
| `MANIFEST.json` | dump path/sha256/size, doc_count, corpus_bytes, separator counts, all three sidecar sha256s |

**What to expect at full scale** (recorded run, this project's own corpus —
**NOT RE-RUN HERE**: 26.9 GB through `bzip2 -dc` takes ~65 min):

```
docs=25,932,219   corpus_bytes=100,093,934,466   separators=25,932,219
seps.bin=106,970,452 B   titles.bin=830,122,239 B
corpus.bin sha256=b9df78709446ef956c963fd0a2536cdaf5fe6e2e89e1f9c6ada6b9e38ee082e1
xsa corpus: one streaming pass, wall 3,883 s
```

(`/mnt/nvme2n1/erikg/wiki/MANIFEST.json`.) Note `separators == docs` — that is the
gate, not a coincidence.

### The rule it enforces

`0x1E` frames documents, so it may not appear inside content. One occurrence and
the pass refuses, publishing nothing:

```sh
xsa corpus --wikipedia fixture_0x1e.ms.bz2 --out out1e
```

```
xsa: error: 0x1E occurs inside document content (count=6 docs=5); rerun with --separator none
xsa: note: 1 of those 6 0x1E bytes came from document content (1 document(s) affected)
xsa: note: nothing was published; the incomplete sidecars keep their .partial names
```

```
$ ls out1e
corpus.bin.partial  seps.bin.partial  titles.bin.partial      # all 0 bytes
```

`--separator none` is the escape hatch: it keeps the byte as content and leaves
`seps.bin` as the only boundary record. **If you use it, the corpus is no longer
0x1E-framed and `xsa build`'s document contract no longer holds** — for a
Wikipedia dump, always fix/`--separator 0x1e` instead.

---

## 6. BUILD THE INDEX

```sh
xsa build --tape --input <dir>/corpus.bin --scratch <dir> \
          --threads N --memory-gb N
```

Real invocation (150 MB corpus, `/dev/shm/t150m.txt`, n = 150,000,001, scratch on
`/dev/shm`, 16 threads, 24 GiB cap; **phase walls `0:53.25` / `0:53.20` / `0:53.99`,
exit 0**):

```sh
xsa build --tape --input /dev/shm/t150m.txt --scratch /dev/shm/repro-verify/s150 \
          --threads 16 --memory-gb 24
```

Tail of stdout:

```
V2_CONTAINER_PUBLISHED path=/dev/shm/repro-verify/s150/t150m.sxi2 bytes=83343688
PEAK_DISC_USED_BYTES 328699377 (du-sampled every 2s across the concurrent producer+finish window)
PEAK_INTERMEDIATE peak_bytes=328699377 n=150000001 peak_intermediate_over_n=2.1913 lcp_source=sorter-bands pending_bands=8
XSA_BUILD_DONE chi=0 pipeline=v2tape container=/dev/shm/repro-verify/s150/t150m.sxi2
```

**Output ends with `XSA_BUILD_DONE chi=<n>`** — that line is the success
certificate. `chi=0` means *chi was not constructed* (see the chi policy below),
not "chi is zero".

### Byte-exact corpora

For LM or any byte-level work, feed the raw file directly to
`xsa build --tape --input raw.bin --scratch fresh-dir` and skip XML entirely.
The tape route preserves every input byte and adds only a virtual END; no XML
wrapping, terminal byte, or entity expansion is needed. `--snap-1e` explicitly
opts into the document-boundary check for framed Wikipedia prefixes.

The XML corpus path copies title and text source bytes verbatim and performs
**no entity expansion**: `&amp;` remains the five bytes `&amp;`, not `&`.
The current 8-bit transport supports at most 255 distinct corpus bytes; a file
containing all 256 byte values is refused because END needs a separate symbol.

The XML MANIFEST declares `entity_unescape=none`. It also adds title/newline/document
framing, so use the raw tape route for byte-exact indexing of an existing file.

### The flags that matter

- `--tape` / `--bin` — **which SHAPE the run is**, and therefore which shape the
  fit check prices. `--tape` is the band sorter (the product route); `--bin` is
  the bin producer, which is also the default. Passing both is refused: two
  shapes, one price. The plan's first line says which one it planned
  (`pipeline=v2tape` / `pipeline=v2bin`), and `XSA_PLAN_DISC_MODEL` states the
  basis of the number that gates the run:

  ```
  XSA_PLAN_DISC_MODEL shape=tape factor=1.00 measured_basis=0.789n@pile-head-100GB base=peak_scratch_measured covers=band-window+container+journal container_inside_scratch_peak=1 ...
  XSA_PLAN_DISC_MODEL shape=bin factor=1.00 base=all_phase_peak(phase_rows) measured_basis=terms-at-1GB-G2 ...
  ```

  The tape shape's scratch peak is the **measured** envelope — 0.789 n on the
  Pile's first 100 GB (`tools/binbwt/RESULTS_PILE_100GB.md`: 78,880,730,679 B at
  n = 100,000,000,001) — carried at `factor=1.00`, i.e. `peak = 1.00 n`. The
  factor covers the band window, the container being published and the journal
  (the container is *inside* the scratch peak, not an addition to it), and it
  does **not** shrink with `--threads`: a box that cannot hold `1.00 n` of
  scratch cannot run the tape route at any thread count. The bin shape is priced
  separately (its tree: rlebwt 1.546 n + the bounded piece window + the container,
  plus the extrapolated witness spill); the two numbers are NOT interchangeable,
  and a plan never gates one shape on the other's figure. The other shape's
  number is still printed, but only as an explicitly labelled
  `XSA_PLAN_ALTERNATIVE shape=bin ...` / `shape=tape ...`.
- `--input` — raw corpus bytes; END is virtual. Add `--snap-1e` only when
  requiring a final `0x1E` document boundary.
- `--scratch` — intermediates, container, `xsa-build.log`, and `checkpoint.journal`.
  Start with a fresh directory, or reuse a matching tape checkpoint for verified
  replay ([RESUME.md](RESUME.md)). Budget ~2.3 n plus the checkpoint journal.
- `--threads` — sort parallelism **and** the number of bands live at once
  (`bands_in_flight = threads`). Raising it shrinks `rows_per_band` for a fixed
  `--memory-gb`, so it is a memory knob as much as a speed knob.
- `--memory-gb` — the hard `RLIMIT_AS` for every phase, and the number the band
  arena is derived from (arena = `memory_gb/2` GiB, 85 % usable). The plan is
  journaled and the front door **refuses the run** if the plan cannot prove a fit.
- `--skip-fit-check` — run despite the **absolute peak-vs-free** scratch check,
  and nothing else. It is journaled with numbers, not an adjective:

  ```
  DISC_CHECK_SKIPPED basis=operator shape=tape peak=1307910802540 peak_gb=1218.09 free=1068785217536 free_gb=995.38 short_gb=222.70 short=222.7 GiB override=--skip-fit-check note=only the absolute peak-vs-free check is skipped; the RAM term, the band law and every receipt still refuse this run
  ```

  It does **not** touch the RAM term (`XSA_PLAN_RAM`: the band arena, the
  `rows_per_band >= 65,536` fold floor, the finish's witness cap), the band law
  (`TAPE_PLAN_DISAGREE`) or any receipt, and it cannot buy the bin shape's 2.5 n
  output-side mission bound. `--allow-infeasible` is a **silent alias** kept so
  old journals, scripts and gate commands still parse and mean the same thing.
- `--plan-only` — survey, model and print, then exit 0. It never starts a phase
  and never refuses: the refusal belongs to the real run, and the verdicts
  (`XSA_PLAN_DISC`, `XSA_PLAN_RAM`) are printed either way, so a preflight cannot
  look like a pass when it is not.
- **There is no `--timeout` and no `--stall-seconds`.** Both are rejected as
  unknown options. A wall-clock cap killed a HEALTHY 100 GB run at 61/102 waves
  after 9.4 h (rc=124) because someone set a work budget, and the progress
  watchdog that replaced it was the same class of error: it killed on a heuristic.
  Neither exists any more - the machinery, not just the default, is gone.

> **No timeouts, no watchdogs: an expensive run ends when it exits, fails, or a
> human stops it.** Every phase is spawned directly: no `timeout` wrapper, no
> monitor process, no stall detection, no kill path. A phase may legitimately sit
> at zero CPU and zero I/O for a long time while it waits on its sibling
> (`lce_wait`), stages a monster band, or waits for a `.consumed`/`.done` handoff,
> so guessing is not allowed. The only signals the driver sends are the cleanup
> `SIGKILL` to a phase's private group when that phase has ALREADY failed (a
> nonzero exit, an OS error) or when the operator interrupts the driver. Watch the
> journal (`SS_BAND`, `XSA_FINISH_PROGRESS`); if a phase is genuinely wedged, stop
> it yourself, with the evidence in front of you.

Sanity-check the plan before spending hours: add `--plan-only` (prints and exits,
0.1 s).

### Progress is journaled (numbers, not prose)

`<scratch>/xsa-build.log` is one file: the plan, the phase argv, then counters.
The lines to watch, all real from the run above (each is ONE line in the journal;
the long ones are shown here in full):

```
TAPE_PRODUCER_BUDGET SS_TAPE_BUDGET memory_gb=24.00 budget_bytes=12884901888 bytes_per_row=90 bpr_terms=rows:32+pingpong:32+lcp:16+slice:1+stage:9 safety=0.85 bands_in_flight=16 rows_per_band=7190595 rows_per_band_budget=7190595 rows_per_band_window=7190595 binding=budget bands=1 waves=21 rows_per_wave=7190595 arena_bytes=10354457665 arena_planned_rows=7190595 cap_rows=115049529 fixed_bytes=166316048 stage_bytes=536870912 source=operator --budget-bytes k_source=thread count (P = --threads; K=--pending-bands is the tape window) fd_soft=1048576 fd_hard=1048576 parallel_bands=16 live_rows=115049520 budget_rows=143165576 ratio=0.8036
TAPE_BUDGET_OK rows_per_band=7190595 bytes_per_row=90 bands_in_flight=16 plan=run=1 budget_ratio=0.1000 bound=0.90
SS_BAND band=0 rows=1800798 read_s=0.119424 sort_s=0.591829 write_s=0.018305
SS_BAND band=2 rows=1425075 read_s=0.084555 sort_s=3.245368 write_s=0.025070
…
XSA_FINISH_PROGRESS band=1/2289 runs=131070 chi_so_far=0 repair_wall=0.7 lce_wall=0.0 lce_wait=0.0 advance_wall=0.0 wit_spills=0
XSA_FINISH_PROGRESS band=833/2289 runs=54657022 chi_so_far=0 repair_wall=50.7 lce_wall=50.5 lce_wait=0.0 advance_wall=0.0 wit_spills=0
```

- `SS_TAPE_BUDGET` — the band size the producer computed, and that it equals the
  plan (`TAPE_BUDGET_OK rows_per_band=… plan=run=1` follows).
- `SS_BAND` — one line per sorted band: rows and wall. This is where band skew is
  visible (§10). 46 lines at 150 MB (median `sort_s` 1.33 s, max 3.55 s).
- `XSA_FINISH_PROGRESS` — the finish's band cursor (`band=i/total`), run count so
  far, and its own wall breakdown; it must advance, and `lce_wait`/credit waits are
  the fusion's stall time.
- `V2_TIMEOUT none ...` — the policy, journaled per run: no wrapper on any
  phase and no supervisor process. `FINISH_POLL_TIMEOUT` is the finish's own
  patience for the producer's next piece (~31 years), never a cap on the run.
- `XSA_SOURCE git_commit=… git_dirty=… branch=…` is the **first** line: which tree
  built this container. This run: `git_commit=6dd06ef6e141a03522c5929d1e411cca468c7c01
  git_dirty=0 branch=reproduce-guide`.

### What the `emit-tape` phase actually does (and what it no longer does)

`xsa build --tape` spawns three processes: `order-tape` (the band sorter,
`scatter_sort`), `emit-tape`, and `finish-v2`. The middle one states its job in
the journal, once per run:

```
EMIT_TAPE phase=emit-tape job=band-concat order=rank tool=cross_lcp_merge cross_lcp=0 seam_pass=none lcp_source=sorter-bands (concatenate the sorter's rank-ordered band slices into the merged stream; the LCP column arrives as band slices and is folded by the finish)
```

**`emit-tape` concatenates.** The sorter publishes, band by band, the next
`rows_per_band` rows of the suffix array in exact rank order (`band_<b>.sa`),
each row's BWT byte (`band_<b>.sym`) and the band's LCP slice (`band_<b>.lcp`).
`emit-tape` reads those bands in band order, re-cuts them into the run pieces the
finish folds, and releases each band as its piece is published (the `.consumed`
handshake, which is the route's only flow control). It does **not** compute an
LCP column, does not compare two suffixes, and does not repair a seam: the order
it streams *is* the order a BWT needs, and the LCP column is the sorter's own
(`lcp_source=sorter-bands`).

The binary keeps the name `cross_lcp_merge` because it also serves the legacy
four-file merge, where cross-LCP work is real. On this route it also journals

```
EMIT_SEAM_PASS none reason=vestigial_under_v2tape sink=BinBandSink n=1307910802541 depth=1 raw_classes=0 aggregate_occurrences=0 unique_targets=0 limit_not_used=67108864 work_limit_not_used=20926572840656 scan_bytes_avoided=1307910802541 basis=end_symbol_unique(end_symbol=0) certified_by=TAPE_EMIT_END_ROWS(rows==1)
```

— i.e. the seam census of the terminal suffix is **gone from this path**. That
census walked the occurrence chain `occ_1` = every position holding the terminal
byte, `occ_{d+1}` = the members of `occ_d` whose predecessor matches (one class
per depth). The appended END byte is virtual and its symbol — code 0, handed to
an *absent* byte by the per-corpus remap — occurs exactly once, so `occ_1` has
one member and the chain never takes a second step. The census could therefore
only ever print `raw_classes=0 aggregate_occurrences=0 unique_targets=0`, which
is exactly what the 100 GB build recorded
(`CROSS_SEAM_DISCOVERED n=100000000001 depth=1 raw_classes=0
aggregate_occurrences=0 unique_targets=0 limit=67108864
work_limit=1600000000016`), with `hits=0` on all 1e11 rows and `count=0` repair
exceptions. It cost a full corpus scan (18 min of the Pile attempt's 18:11 wall)
plus a `(n>>6)+2`-word target bitmap — n/8 bytes: 12.5 GB at 100 GB (that run's
whole peak RSS was 13,136,084 KiB), 152 GiB at the Pile, where it was the
`std::bad_alloc` that killed the phase — to learn nothing.

The assumption is not taken on trust, and it is not this phase's assumption to
make. The producer REFUSES to run a corpus where it could fail:
`scatter_sort` dies with `tape consumer reserves cell zero; virtual END consumer
required` if any *present* corpus byte maps to code 0, so the appended END is the
text's only 0 on every accepted run. `emit-tape` then certifies the same fact on
the data it writes: `TAPE_EMIT_END_ROWS rows=<n> symbol=0 expected=1` refuses a
stream whose BWT carries more than one END row. The machinery itself is intact
and reachable: `XSA_SEAM_CENSUS=1` is the INTERNAL A/B arm (never a user option,
never needed for correctness) that runs the real occurrence chain and journals
its own allocation, sized from the census and never from n:

```
SEAM_ALLOC sink=BinBandSink arm=env classes=0 bytes<=0 basis=census limit=<2^26 cap> depth=1 unique_targets=0 note=the occurrence set is bounded by the census limit, never by n
```

The default arm journals the same decision with zero work:

```
SEAM_ALLOC classes=0 bytes=0 basis=census short_circuit=unique_targets_zero
```

**No allocation in the emit phase is a multiple of n.** `tape_emit` allocates
only its chunk buffers (`--tape-emit-chunk`, 4 Mi words + 4 Mi bytes), and the
sink's bands are bounded by the band window. The n-proportional allocations that
remain in `cross_lcp_merge.cpp` are all on the *legacy* arms, which the product
route never spawns: `write_pos_sxp4`'s `stream.reserve(n*4+64)` (the v1 SXP4
order column), the SXCR text reconstruction's `bwt(n)`+`lf(n)` (9 n, the v1
pairwise/k-way pair path) and `anchors.resize(n)` in the cyclic repair; the
`--selftest` oracles' `v.reserve(n)` are oracle-only. If you touch any of them,
price it against the phase's `RLIMIT_AS`, not against n.

### m14, the LCP over runs: OPT-IN (`--with-lcp-runs`), and what it buys

The suffix tree *is* the LCP-interval tree: a node's children are delimited by the
ranks where LCP drops below the node's depth. The finish computes that column for
the chi sweep and used to throw it away. `--with-lcp-runs` persists it as member
**m14** (codec 117, SXI2 **v8**), one exact u64 per BWT **run**, in run order, at
the run's head row: `lcpRuns[i] = lce(SA[r_i-1], SA[r_i])`.

```sh
xsa build --tape --input CORPUS --scratch DIR --threads 24 --memory-gb 24           --with-lcp-runs                      # v8 container WITH m14
xsa build --tape --input CORPUS --scratch DIR --threads 24 --memory-gb 24
                                               # DEFAULT: v7, no m14, byte-identical
```

**DEFAULT OFF, and why.** m14 costs a measured **0.27 n at 150 MB, 0.256 n at
10 GB, 5.89 bits/run** (~354 GB on a full-Pile container). What it uniquely buys is
the MIDDLE band of the two-backend rule (MEMORY_OPS.md): contexts too wide to answer
by `seek`+locate-and-scan and not materialised in the top-of-tree trie, plus node
depths, child delimiters, MEM/interior enumeration. Measured on the 10 GB Pile slice
(1000 byte-weighted contexts per depth, seed 20261110, widths from the container's
own `--count`):

| depth | median width | w==1 | 2..999 | 1e3..1e5 | 1e5..1e6 | >=1e6 |
|---:|---:|---:|---:|---:|---:|---:|
| 8  | 2,815 | 4.3 % | 36.1 % | 45.7 % | 11.3 % | 2.5 % |
| 16 | 3     | 39.9 % | 53.8 % | 5.1 % | 0.0 % | 1.1 % |
| 24 | 1     | 73.8 % | 24.6 % | 0.9 % | 0.1 % | 0.5 % |
| 32 | 1     | 85.7 % | 13.6 % | 0.4 % | 0.0 % | 0.2 % |

At information depths (16..32 B) that band is 0.4..5.1 % of contexts, so m14 is a
quarter of `n` spent on a thin band: opt in when you need topology from the
artifact. The plan line says which arm ran (`lcp_runs=absent|present(codec=117)
member=m14 lcp_runs_version=7|8`), and so does the container's own header (v8 +
flag bit 7 + member m14), so a reader never needs the flag.

**Default-off is byte-identical to v7**: the 150 MB container built with no flag is
sha256 `60b56ffa3502d466b80f0dc8fae2ee960ffc4ec41ec6c2c8d535cc80478689d2`, and every
one of its 8 members is byte-identical (id, codec, bytes, count, crc, sha) to the
`--with-lcp-runs` container's members; only the offsets of m12/m13 move, because m14
sits between m11 and m12.

Reading it back:

```sh
xsa lcp <container> --run I | --runs A:B | --sample N --seed S | --all [--audit]
xsa lcp-verify <container> --corpus CORPUS [--sample 50] [--deep 50]
```

`xsa lcp` answers one run from one 16-byte directory entry + one bit slice + one
escape-list binary search; `--audit` walks the trailing directory (it must tile the
payload exactly) and every block's escape list (which must be sorted). `lcp-verify`
compares stored values against a brute-force LCE over the corpus and exits non-zero
on any mismatch; a container without m14 is refused **by name**, never answered
around. Measured: 124/124 exact at 150 MB, 140/140 exact at 10 GB.

### A POINT-MASS class BORROWS one arena - and may exceed the scratch budget

A wave sorts a class of suffixes by a 24-byte key, and a class that no key byte
splits and that is not the single-symbol tail run has exactly one order-safe shape:
**one wave carrying the whole class** (its rows route to one band, the row gate
admits that band alone, and the band is sorted against the corpus with `suf_cmp`
when the keys tie). Under a tight `--scratch-budget-gb` the wave goal collapses and
such a class no longer fits the plan, so the sorter either borrows or refuses:

```
SS_WAVE_BORROW class_rows=54973083 bytes_per_row=90 arena_gb=4.608 disk_gb=2.099   mem_cap_gb=29.802 wave_goal=7124992 path=borrow-one-wave   reason=point_mass_no_key_byte_splits_it_no_position_slice_is_sound
```

- The borrow is capped by **`--memory-gb`** (the operator's RAM promise), not by the
  band plan; `SS_WAVE_BORROW_TOTAL` reports what was admitted and refuses if OWNED
  RSS passes `--memory-gb`.
- **A capped build may therefore exceed its scratch budget by up to one point-mass
  class** (its band file is `class_rows x 41 B`). That is stated in the journal line
  itself and it is not hidden.
- If the class does not fit `--memory-gb`, the sorter REFUSES by name: a chunked
  sort + order-preserving merge inside one class is the only remaining shape and it
  is not implemented. Never publish an unproved order.

On the Pile's first 100 GB the class is **16+ SPACES**: 763,350,000 rows (the
sorter's sample; an independent corpus census puts it at 762,642,500), i.e. the
Pile's indentation - 41,229,268 space runs of >= 16 B, max 15,992 B, 1.38 % of the
bytes - not a duplicated document and not one monster filler run.

**The fixture that proves the borrow changes no product byte** (150 MB, 4.5 minutes
per arm; the 40 MB version is refused earlier by the 65536-row fold floor, so it
never reaches this path):

```sh
# build the indentation corpus (see gates/lcp_runs_borrow_fixture.sh for the generator)
gates/lcp_runs_borrow_fixture.sh
# -> SS_WAVE_POINT_MASS_CLASS prefix_text='                '   (16 SPACES, corpus bytes)
# -> SS_WAVE_BORROW ... path=borrow-one-wave
# -> tight_sha256 == free_sha256 == e859d53bc56979053206f3ced6b554585dd045f5d63ae5bab817fd0237927cad
# -> GATE BORROW PASS
```

### An EMPTY band is acked, never ignored

The sorter can publish a band with **no rows** (a wave that closes a byte range
without adding a row: `band_<b>.sa` of 0 bytes, `band_<b>.done`, an LCP slice with
`rows=0`). `emit-tape` is what answers it, and the sorter's wave drain waits for
`band_<b>.consumed` before its next wave — and therefore before it publishes
`end.ready`. The phase therefore drains any PUBLISHED band before it asks for the
end marker, and journals the empty case:

```
BAND_EMPTY_ACK band=40 bytes=0 consumed=1 rows_before=4000001
```

Without that ordering the two sides deadlock with the row budget exhausted: the
emit waits for `end.ready`, the sorter waits for the empty band's `.consumed`,
both alive, forever. It was measured at 4 MB (`n=4,000,001`, DNA) and it is not
new — the mainline binary does it too.

### chi policy

The recorded runs below used an older opt-in chi default. Current builds enable
chi (the minimal suffixient set, member m5) by default; pass `--no-chi` to omit it.
Then the journal says `CHI_POLICY present=false member=m5` and the finish receives
`--no-chi`. Exact matching does not read m5, so queries
work either way; container size drops from 0.69 n to 0.56 n at 150 MB.

Request it explicitly with one flag:

```sh
xsa build --tape --input /dev/shm/t150m.txt --scratch /dev/shm/repro-verify/s150chi \
          --threads 16 --memory-gb 24 --with-chi
```

```
V2_CONTAINER_PUBLISHED path=/dev/shm/repro-verify/s150chi/t150m.sxi2 bytes=103727312
XSA_BUILD_DONE chi=44034230 pipeline=v2tape container=/dev/shm/repro-verify/s150chi/t150m.sxi2
```

`--chi-expect N` (the in-process assert) is refused with `--no-chi`:
`--chi-expect requires --with-chi on the tape route (missing member m5)`.

---

## 7. QUERY

```sh
xsa query <container> <pattern> [<pattern> ...] [-j N]
```

Each positional argument is one verbatim byte pattern (quote spaces in the shell;
use `--` before patterns beginning with `-`). Pattern ids start at zero. For example:

```sh
xsa query corpus.sxi2 "domestic cat" "feral cat" -j 32
```

For binary patterns or bulk queries, use `--patterns patterns.bin` with 0x1E-separated
records. Use `--patterns-fasta patterns.fa` explicitly for FASTA. Mixing file and
positional patterns is refused because pattern ids would be ambiguous. `--all`
emits every hit (also the default); `--output jsonl` preserves the machine
format, one object per pattern with `pattern_id`, `count`, and `offsets`.

Use `--count` for interval widths only: one
`pattern_id<TAB>pattern<TAB>occurrences` row per pattern, including absent patterns
(count zero). No positions are located and no LF locate walks run; sidecars are
not loaded. Pattern tabs, newlines, carriage returns and backslashes are escaped
as `\t`, `\n`, `\r`, `\\`; other bytes are preserved. This mode always emits
TSV and refuses `--output jsonl` and the older `--counts` flag (which retains its
existing locate-and-count behavior). Startup still opens the rank index; the
batch search time and whole-process wall time are distinct measurements.

Use `--limit N` to locate at most N occurrences **per pattern** (`N=0` is allowed).
The walk visits the first N rows of each interval in suffix-array **rank order**,
not text-position order. Results are deterministic: no RNG, no seed, reproducible.
This can give dispersed examples for exploring **which documents contain this**,
but does not enumerate all occurrences or all matching documents. Suffix ranks
are lexicographic: following context, repeated text and duplicate documents can
bias the first N rows. Thus it is not a statistically unbiased sample, and spread
in one measured query does not guarantee representative document coverage for
other patterns. For exhaustive occurrence results omit both flags or use `--all`.
`--all` remains an accepted no-op synonym for the default; an explicit limit still
caps output. With `--limit`, JSONL `count` describes the returned offsets; the
journal's `occurrences` describes the complete interval. `--count` and `--limit`
together are refused before the container is opened.

`QUERY_LOCATE mode=count|limit|all positions_located=N lf_steps=N` journals actual
locate work. `QUERY_DONE occurrences=N` reports total interval widths, including
when output is limited. Neither journal appears on stdout.

```sh
xsa query corpus.sxi2 the cat --count
xsa query corpus.sxi2 the --limit 16 --docs /path/to/corpus-sidecars
```

Validated 2026-11-02 (`tools/binbwt/gate_query_count_limit.py`; full journals and
T2 output in `tools/binbwt/query_count_limit_results.json`):

- 150 MB: 100/100 counts exact, 574/574 default positions exact; default output
  byte-identical to the pre-change binary. Three warmed runs: median process wall
  **0.279079 s count vs 0.685596 s full locate (2.457×)**. Median query-kernel
  time is **0.6 ms vs 475.3 ms** (startup excluded).
- 100 GB: 20 count patterns, including `the` (**436,092,525** occurrences),
  **12.5 ms batch search**, **109.043147 s process wall**; journal confirms
  `positions_located=0 lf_steps=0`. Index initialization dominates process time;
  this is not a millisecond whole-process CLI invocation.
- `the --limit 16`: exactly **16 rows across 13 distinct documents**,
  **130.483590 s process wall** (39.6 ms search + locate); all 16 offsets checked
  directly against corpus bytes. This demonstrates dispersion, not unbiasedness.
- Limit 16 on a one-hit pattern is byte-identical to unlimited output; all 100
  limit outputs match the corresponding full-output prefixes. Modern 10 GB
  oracle: 20/20 counts and positions exact.

Default TSV discovers document sidecars in this order: `--docs DIR`, next to the
container, then the corpus directory recorded in its adjacent `xsa-build.log`.
New builds journal `CONTAINER_CORPUS_SIDECARS` with a JSON-quoted absolute directory,
sidecar filenames, doc count (zero if unavailable), and corpus byte count. Historical journals use their
absolute `XSA_BUILD START input=` path. Keep the journal beside a moved container,
or move its sidecars beside it, or supply `--docs`. Candidates must agree with the
container byte count and the sidecars' counts (and manifest, when present); a recorded doc count is also
checked when available. Invalid candidates warn and discovery continues.

With matching sidecars, rows are `pattern_id<TAB>doc_id<TAB>offset_in_doc<TAB>title<TAB>global_offset`.
Titles escape backslashes as `\\` and newlines as `\n`. Without matching sidecars,
a warning accompanies the original `pattern_id<TAB>global_offset` rows.
`QUERY_DOCS source=docs|sibling|recorded|none docs=N corpus_bytes=N` records the choice
on stderr. Historical containers do not carry an independent document count; their
sidecars' counts are checked against one another and their manifest when present. Matching counts
and sizes are metadata checks, not a cryptographic corpus identity check.

Archived bulk invocation and output (`-j 4`, 21 patterns on the 150 MB container):

```sh
xsa query /dev/shm/repro-verify/s150/t150m.sxi2 --patterns patterns.bin -j 4
```

stdout (first lines; full output is below):

```
0	1000
1	7401011
```

stderr (all of it — the counters, not the answers):

```
QLITE_OPEN container=/dev/shm/repro-verify/s150/t150m.sxi2 format=sxi2-v6-lite magic=SXI2 version=6 mmap_ms=0.07 decoded_bytes=0 stages=none n=150000002 runs=57279194 members=m1,m4,m7,m10,m11,m12,m13 end_symbol=none orientation=none alphabet=none
SQLITE_QUERY_OPEN format=sxi2-v6-lite mmap_ms=0.07 decoded_bytes=0 stages=none
PATTERNS_USED path=patterns.bin format=records-0x1e detected=no-leading-gt records=21 bytes=525 ids=0..20 separator=0x1e
QLITE_BUILD runs=57279194 decoded_bytes=524887754 stages=[m11-anchors=0.4ms prefetch=0.1ms run-columns=152.2ms] build_ms=152.7 n=150000002 threads=4 patterns=21 m1_block_runs=1024 m1_blocks=55937 rundir_l2=512 rundir_occ_stride=256 rundir_gap=1024
QUERY_DONE patterns=21 occurrences=21 absent=1 query_ms=5.9 first_answer_ms=159.9 low_count_patterns=21 low_count_mean_us=848.1 low_count_max_us=2544.9 low_count_lf_steps=32841 low_count_mean_steps=1564 low_count_max_steps=4810 high_count_patterns=0
```

Full stdout — 21 occurrences over 21 records (one record absent, so it contributes
no line):

```
0	1000
1	7401011
2	14801022
3	22201033
4	29601044
5	37001055
6	44401066
7	51801077
8	59201088
9	66601099
10	74001110
11	81401121
12	88801132
13	96201143
14	103601154
15	111001165
16	118401176
17	125801187
18	133202325
18	133201198
19	140601209
```

### Output contract

`id<TAB>offset`, one line per occurrence, **no header**, absent patterns produce no
line. A pattern containing any byte absent from the corpus produces zero
occurrences, including bytes whose remap slot names virtual END. The present set
is recoverable from container members m7 (byte remap) and m1 (symbol counts):
`present[b] = sigma[b] != 0 && count[sigma[b]] > 0`. Builds journal it as
`CORPUS_PRESENT_BYTES count=... hex=...`; queries journal rejected pattern counts
as `QUERY_ABSENT_SYMBOL patterns=...`.

`n=150000002` in `QLITE_OPEN` is `corpus_bytes + 1`: the container indexes a
virtual END row. Nonempty byte patterns return only **corpus offsets**
(0..corpus_bytes−1), never the virtual END position.

**Offsets within one id are NOT sorted.** Record 18 above is emitted
`133202325` then `133201198`: occurrences come out in suffix-array **rank** order.
If you need text order, `sort -k1,1n -k2,2n`.

`--output jsonl` gives `{"pattern_id":i,"count":c,"offsets":[…]}` per record;
`--counts` replaces the tsv body with `id<TAB>count`; `-j` defaults to all cores.

### Pattern file format

The **same contract as the corpus**: records separated by `0x1E`, raw bytes, no
escaping, no alphabet, and `id = 0-based record index`:

```
rec \x1E rec \x1E … rec \x1E        (one terminal separator terminates; it does not open a record)
```

- arbitrary bytes allowed (this is a byte index, not a DNA index) — this project's
  own oracle patterns contain NULs and `0x1E`-free 24-byte windows of raw text;
- a record may not be empty (`record 1 is empty; the 0x1E contract has no empty
  record …`) and may not exceed **65,536 bytes** (`record 0 is 70000 bytes, over the
  65536-byte pattern limit`);
- `--patterns-fasta <file>` reads FASTA (one sequence per record, newlines
  dropped). `--patterns` always reads raw 0x1E records, including a leading `>`.
  There is **one orientation only** — no reverse complement is ever searched.
- **`--ri4` no longer exists** (nor `--sxi`): the container is positional and
  identifies itself. `--ri4` is refused by name, and so is `--mode` (a lite
  container has no alphabet).

### Verified against a brute-force oracle

The 21 records above were also counted by an independent C scan of the corpus
(`oracle.c`: one full pass per pattern, `memcmp`, no index). The two agree
exactly:

```
$ diff <(sort -n -k1,1 -k2,2 oracle.tsv) <(sort -n -k1,1 -k2,2 q.out) && echo IDENTICAL
IDENTICAL          # 21/21 occurrences, 21/21 ids, 1 absent (id 20 = 24 × 0x01)
```

---

## 8. EXPECTED NUMBERS

Container bytes, run counts and chi for the two banked rungs. The 150 MB row was
re-measured here; the 10 GB row is from the project's own result files (a 10 GB
build was **NOT RE-RUN** for this guide).

| corpus | n | runs r | chi (`--with-chi`) | container, chi present | container, chi absent |
|---|---|---|---|---|---|
| 150 MB `t150m.txt` | 150,000,001 | 57,279,194 | **44,034,230** | **103,727,312 B** (0.6915 n) | **83,343,688 B** (0.5556 n) |
| 10 GB `pile-10b-snap.txt` | 10,000,017,259 | 3,474,889,847 | **2,676,929,445** | **6,178,186,696 B** (0.6179 n) | **4,883,953,184 B** (0.4884 n) |

Sources: the 150 MB row is **re-measured here** (my `--with-chi` build produced
exactly `103,727,312` and the default build exactly `83,343,688`, which also
settles that the JSON's `container_before_bytes`/`container_after_bytes` mean
with-chi/without-chi); 10 GB "chi absent" = `tools/binbwt/optional_chi_results.json`
N2 (`n=10000017259 runs=3474889847 chi=0 container_bytes=4883953184`); 10 GB
"chi present" container = `tools/binbwt/RESULTS_QLITE_BLOCKM1.md` and
`tools/binbwt/RESULTS_DEADLOCK_REDESIGN.md` (`CONTAINER 6,178,186,696 B = 0.62 n`,
with `r=3,474,889,847` and chi `2,676,929,445`).

Two honest footnotes:

- **chi is a property of the text, so it must be reproducible; the container is
  not.** The 150 MB chi printed above (`44034230`) is byte-for-byte the banked value
  in `chi-optional/n1-150-on/finish.log`. An older, pre-virtual-END format printed
  `44,034,231` and `2,676,929,428` for the same corpora — if you see those numbers
  you are on a different container version, not looking at a bug.
- Query cost at 150 MB: open 0.07 ms (mmap only), kernel build 153 ms, then
  ~0.85 ms/pattern for low-count patterns, 21 patterns in 5.9 ms at `-j 4`.

---

## 9. WHEN IT REFUSES

Every row below is a real message from this tree; the ones marked ✔ were produced
by running the command during this guide. This section is the point of the
document: the tool refuses loudly and early, and each refusal names the knob.

### `xsa corpus`

| message (verbatim) | means | change |
|---|---|---|
| `0x1E occurs inside document content (count=6 docs=5); rerun with --separator none` ✔ | a raw `0x1E` in a body would break document framing; nothing published (only `*.partial` left) | fix the source, or accept `--separator none` (and lose 0x1E framing) |
| `corpus: <file> does not start with 'BZh': not a bzip2 stream (pass the compressed dump)` ✔ | you handed it XML, or a `.xml` that merely looks bz2-shaped | pass the `.bz2` (or `-` with the compressed stream on stdin) |
| `corpus: <path> already exists; refusing to mix a new pass into an old directory` ✔ (`…/corpus.bin already exists…`) | output dir is not fresh | use a new `--out`, or move the old pass aside |
| `corpus: decompressor failed (exit status: 2): truncated or invalid bzip2 stream; refusing to emit a short corpus` | the bz2 stream is truncated/corrupt — **never** a short corpus | re-download; verify sha1 |
| `corpus: truncated or malformed XML: stream ended at offset 900 with <revision> still open` | decompressed XML is truncated | the dump is incomplete |
| `corpus: malformed XML: <text> at offset 104 is not a direct child of <revision>` / `page 'X' has no <text> element` / `page has two <title> elements (second at offset 94)` | the input is not MediaWiki pages-articles shape | feed a real dump |
| `corpus: the dump yielded 0 documents; refusing to emit an empty corpus` | valid bz2, no `<page>` | wrong file |
| `corpus: no bzip2 decompressor on PATH (tried pbzip2, bzip2; …)` | neither binary found | install `bzip2` |
| `corpus: more than 2^32-1 documents: the u32 delta/offset sidecars cannot address them` | sidecars are u32-indexed | split the corpus |
| `corpus: unknown arg <x>` | e.g. the removed `--max-bytes` | there is no byte budget: slice the dump with `head`, not the indexer |

### `xsa build`

| message (verbatim) | means | change |
|---|---|---|
| `SNAP_REFUSED mid-document input <path>: last byte is 0x20, want 0x1e (document boundary); snap the prefix forward to the next 0x1e in the source corpus and retry - the chain never trims or extends silently` ✔ | `--snap-1e` requested but input does not end on a document boundary | omit `--snap-1e` for raw tape input, or choose an aligned prefix |
| `PLAN_INFEASIBLE: <the XSA_PLAN_DISC / XSA_PLAN_RAM verdict, verbatim>` then `The XSA_PLAN_ALTERNATIVE lines above name the knobs; --skip-fit-check overrides ONLY the absolute peak-vs-free check (it does NOT touch the RAM term)` ✔ | the plan could not prove the run fits scratch or RAM. The verdict names the **shape**, the peak vs the limit **in the same unit and in bytes**, the **shortfall**, the exact override flag and the knob that fixes it - every refusal is one self-contained sentence | read the shortfall: free scratch space, raise `--memory-gb` (the band arena is half of it), lower `--threads`, or plan the other shape (`--bin` / `--tape`); `--skip-fit-check` only if you know the box fits |
| `XSA_PLAN_DISC INFEASIBLE shape=tape: the tape route's measured scratch peak (band window + container + journal): peak 1218.1 GiB (1307910802540 B) vs free scratch 995.4 GiB (1068784988160 B) -> SHORT 222.7 GiB (22.4% over); override --skip-fit-check to run anyway ...` ✔ | the tape route's scratch peak (`1.00 n`, from the measured `0.789 n`) does not fit this scratch device. This is a *measured* n-multiple: `--threads`/`--memory-gb` do not shrink it | free scratch, point `--scratch` at a bigger device, or `--skip-fit-check` if the device really has room |
| `XSA_PLAN_DISC INFEASIBLE shape=bin: the bin shape's ALL-PHASE scratch peak (rolling window FILES + rlebwt + piece window + container + witness spill): peak 134.2 MiB (140702137 B) vs free scratch 5.0 MiB (5242880 B) -> SHORT 129.2 MiB (2583.7% over); ...` ✔ | the BIN shape's tree does not fit (this one is a 5 MiB tmpfs and a 4 MiB corpus); sub-GiB limits print in MiB so the shortfall is not rounded to `0.0 GiB` | same knobs, or `--tape`, whose peak is the measured `0.789 n` window rather than the tree |
| `XSA_PLAN_RAM INFEASIBLE shape=tape RAM: the band arena is ... rows_per_band=1 floor=65536 (short 65535 rows); override=none: no flag skips the RAM term, --skip-fit-check does NOT touch it; fix: raise --memory-gb until the usable envelope covers 1.2 GiB` ✔ | the band arena cannot hold 65,536 rows per band, or its peak is over 90 % of the envelope. `--memory-gb` (and `--threads`, which divides the arena) is the only lever | raise `--memory-gb`, or lower `--threads`; **no flag overrides this** |
| `DISC_CHECK_SKIPPED basis=operator shape=tape peak=... free=... short_gb=... override=--skip-fit-check ...` | `--skip-fit-check` (or its alias `--allow-infeasible`) was used: the absolute peak-vs-free check was skipped, with both sides of the comparison journaled | nothing to do - the run continues; if `short_gb` is large, the run may still die on a full device |
| `--tape and --bin are mutually exclusive: they are two shapes (a band sorter vs the bin producer), and the fit check prices exactly one of them. Pass one` ✔ | both shapes asked for in one command | pass exactly one (`--bin` is the default, so usually just drop `--bin`) |
| `XSA_PLAN_RAM INFEASIBLE: … rows_per_band=59 < floor=1004 …` ✔ | band arena ÷ (bytes_per_row × threads) fell below the 65,536-row floor | **see the minimum-corpus note below** |
| `scratch already used: <dir>/xsa-build.log exists; move the previous run away (never overwrite)` | legacy routes still require fresh scratch | new scratch dir |
| `RESUME_REFUSED field=<field> ...` | tape recovery identity differs, or a legacy scratch has no recovery PLAN | restore the original input/binary/options, or explicitly use `--no-resume` |
| `BUILD_INTERRUPTED reason=<sig> last_durable_piece=<i> scratch=<dir>` | graceful tape interruption, exit 75 | repeat the same command to replay verified input |
| `scratch is owned by a live build or stage: <dir> (<pid>)` | an inherited advisory lock says a live process owns it — this is the guard that stops two builds colliding in one scratch (the `CROSS_PIECE_COLLISION` class of bug) | never recreate/unlink the lock; use another scratch |
| `--chi-expect requires --with-chi on the tape route (missing member m5)` | an assert you cannot satisfy without the member | add `--with-chi`, or drop `--chi-expect` |
| `V2_CHI_MISMATCH chi=<got> expected=<want>` | the in-process assert failed: the container's chi is not the banked constant | do not ignore; the build is not byte-reproducing |
| `PHASE <tag> FAILED (rc N); log tail: …` | a native stage died; the tail is pasted | read `<scratch>/<tag>.log` |
| `TAPE_AB_ARM: XSA_LCE_VERIFY=1 but the producer ran <arm>` | the A/B arm was requested but the producer chose the other LCP source | unset `XSA_LCE_VERIFY` for the product route |
| `TAPE_LCP_RESIDENCY: max_live_lcp_slice_bytes=<x> > n=<n>: a slice must be ONE BAND's, not the whole column` | an LCP slice grew to corpus size — the bounded-memory law is broken | stop; this is a regression, not a knob |
| `V2_BIN_ROW_COVERAGE emitted_rows=<x> != n=<n>` / `V2_BIN_SWEEP_CONSERVATION …` / `V2_BIN_LAW full_SA/persistent_positions must both be 0` | the producer's own receipts disagree with the corpus | stop; regression |
| `V2_BIN_CAP_OVER_BUDGET: --bin-live-cap <v> cannot fit the phase budget: …` | an explicit `--bin-live-cap` is too large for `--memory-gb` | lower the cap or raise `--memory-gb` |
| `REMAP_REFUSED histogram total <x> != corpus bytes <n>` / `REMAP_REFUSED not a bijection: code <v> is named twice` | the corpus-derived remap did not validate | stop; regression |
| `BUNDLE_TOOL_MISMATCH <name>: <path> sha256=<a> expected=<b>` | the extracted stage cache is corrupt or stale | `rm -rf ~/.cache/xsa/<manifest-sha256>` and re-run |
| `--bin-live-cap is a bin-pipeline flag` / `<knob> needs --phrase-radix` / `CROSS_V2_PRODUCERS=<v> is INERT under the bin shape (…)` | a knob belongs to a different route | drop it, or select that route |
| `--chunks is INERT under the bin pipeline (the default; the producer sweeps the corpus itself, there is no chunk phase); drop the flag, or pass --legacy-merge for the chunk+merge route` ✔ | same: `--chunks/--chunk-mb/--kway/--fanin` are legacy-merge only | drop them |
| `unknown option <x>` | the tape route accepts `--input --scratch --tape --bin --threads --memory-gb --plan-only --skip-fit-check --with-chi --chi-expect --finish-as-gb --wit-ram-gb --snap-1e --remap --stripe` (`--with-lcp-runs`, `--no-lcp-runs`, `--scratch-budget-gb`, `--finish-as-gb`; `--allow-infeasible` is the alias of `--skip-fit-check`; the `--bin-*` and `--phrase-*` arms are ignored/tuned per route) | — |

**The tape route has a minimum corpus size (~1.1 MB).** The band size is bounded by
the declared 2 n scratch window: `rows_per_band ≈ 0.0594 n`, which must reach the
65,536-row floor. Measured here with `--plan-only`:

```
size=1103000 rows_per_band=65490 XSA_PLAN_RAM INFEASIBLE
size=1104000 rows_per_band=65550 XSA_PLAN_RAM FEASIBLE
```

So: a 1 KB "hello world" corpus cannot be indexed by `--tape` no matter how much
`--memory-gb` you give it. Use the 150 MB fixture (or ≥ ~1.1 MB) to exercise the
pipeline. The legacy `--text` entry is retired and refuses immediately with a
pointer to `xsa build --tape --input`; tiny tape inputs remain unsupported.

### `xsa query`

| message (verbatim) | means | change |
|---|---|---|
| `query: --ri4/--sxi are gone; the container is positional and identifies itself` ✔ | old CLI shape | `xsa query <container> --patterns <file>` |
| `query: --mode is not accepted for a lite container: … no reverse complement` ✔ | DNA-style orientation does not exist on a byte index | drop `--mode` |
| `<file>: unrecognised file (magic 49742069) is not a queryable index` ✔ | you passed text or an unrelated file | pass the `.sxi2` |
| `<path>: record <i> is empty; the 0x1E contract has no empty record (and no escaping: a bare 0x1E is always a record boundary)` ✔ | `a\x1Eb\x1E\x1Ec\x1E` — two separators in a row | remove the empty record |
| `<path>: 0x1E record file holds no record (0 bytes)` ✔ | empty pattern file | — |
| `<path>: record <i> is <k> bytes, over the 65536-byte pattern limit` ✔ | a 70,000-byte "pattern" | split it (a pattern is a read, not an assembly) |
| `query: --counts is a tsv option; --output jsonl already reports a count per id` ✔ | flag conflict, refused before any work | pick one |
| `query: unknown option <x>` | unrecognized option | use positional patterns or one pattern file; use `--` for leading hyphens |
| `<path>: <e> … supported: SXI2 v6 lite …` | a legacy/`.ri4` container | rebuild it with `xsa build`; `query-legacy` is deprecated and ~1e6× slower per pattern |

### Native-stage refusals you can still see through `xsa build`

| message (verbatim, from the stage logs) | means | change |
|---|---|---|
| `CROSS_PIECE_COLLISION path=…/p-<band>-<shard> existed_at_start=0 … piece exists; refusing to clobber` | two pipelines wrote one piece directory — the scratch-ownership bug class | one build per scratch, always; the front door now holds the lock |
| `band exceeds bounded reorder row budget; reduce work unit` (exit 2) | a band grew past the 268,435,456-row / 2 GiB reorder allowance | lower `--memory-gb`/`--threads` so `rows_per_band` shrinks, or re-plan |
| `SS_LCP_CREDIT_ROWS below one fold band (65536 runs) would deadlock the fold` | a knobs combination that cannot make progress | use the defaults |
| `no consumer progress on slice.lcp.consumed band_<i> …` | the finish stopped folding slices | stop; report the pair of logs |
| `run index exceeds u32` (`bit6/xsa_finish.cpp`, `pf_check`) | the container's m13 sampled run table is u32; a corpus with > 4.29 e9 runs cannot publish | see §10 |

---

## 10. KNOWN LIMITS

- **chi peak RSS ≈ 1.2 n, and it is the biggest single process.** The operator's
  observed peak is **13.4 GB at a 10 GB corpus** for the chi-present finish (figure
  supplied by the project operator, not re-measured here); the repo's own JSON
  records `finish_peak_rss_kb=11,781,604` (11.2 GiB, `finish_peak_rss_over_n=1.206`)
  and `finish_proc_VmHWM_kb=11,996,240` (11.4 GiB) for the same 10 GB rung
  (`tools/binbwt/optional_chi_results.json`, N2). Both readings say **~1.2 n**, and
  both run under the same `--memory-gb` cap as the producer — budget ≥ 1.5 n of RAM
  for a chi build at 10 GB and beyond. That is why chi is opt-in.
- **Band skew is the latency tail.** Bands are sized from RAM, not from the text, so
  a locally dense region produces a slow band and the finish waits for its piece.
  Measured spread: at 150 MB, 46 bands, `sort_s` median **1.33 s**, max **3.55 s**
  (this guide); at 10 GB, over the 353 bands a stopped run completed, p50
  **2.10 s**, p95 **11.37 s**, max **322.07 s**, and the worst credit wait was
  **294.62 s** (`RESULTS_DEADLOCK_REDESIGN.md`, P3). p99 is the tail, not the mean.
- **m13 (per-symbol sampled run directory) is u32-wide.** Publication requires every
  sampled run ID `< 0xffffffff` (`bit6/xsa_finish.cpp`: `pf_check(… "run index
  exceeds u32")`, recorded in `bit6/SXI_FORMAT.md`, commit `6dd06ef`). At 10 GB
  r = 3.47 e9 (fits); scaled to the 100 GB corpus, r would be ≈ 3.5 e10 (extrapolated
  at constant run density, not measured), so **a full 100 GB container cannot be
  published in this format** — the 100 GB gate is a construction-progress gate, not
  a product. Widening m13 is a format change.
- **A 100 GB build takes ~18 h** (the tape machine was measured at ~50 s/GB for the
  SA alone, `RESULTS_TAPE_PROTOTYPE.md`: "1.31 TB ⇒ ~18 hours for the SA alone, not
  the ~3 h I predicted. That is a 6× miss, recorded here rather than quietly
  revised."). The longest completed run at that scale is a partial progress gate
  (44225/1527313 finish bands, 1819 s, peak finish RSS 100 GB, `n=100093934467`;
  `optional_chi_results.json` N3). Budget a day, not an hour: there is no cap and
  nothing kills a phase, so plan the wall you actually need.
- **The crate compiles a snapshot that is not always the repo's copy.**
  `xsa/runtime/tools/binbwt/scatter_sort.cpp` (130,934 B, contains the
  `SS_TAPE_BUDGET` law) and repo `tools/binbwt/scatter_sort.cpp` (87,303 B, no
  budget law) **differ at HEAD `6dd06ef`**, and `tools/package_xsa_sources.py`
  copies repo → snapshot. Running the packager therefore *regresses* the shipping
  sorter. Until that is reconciled, treat `xsa/runtime/` as the compiled source of
  truth and diff before you package.
- **Single orientation, byte semantics.** The index has no alphabet: no reverse
  complement, no IUPAC, no case folding. Two orientations of a DNA read are two
  different byte strings and both must be queried explicitly.
- **`--scratch-free-pct` is inert.** It is deprecated and forced to 0, and
  `disc_preflight` returns immediately at 0, so the `DF_GATE_FAIL …` message and the
  "scratch hygiene policy" alternative are currently unreachable. The only disc
  refusal is a genuine will-not-fit (`XSA_PLAN_DISC INFEASIBLE` → `PLAN_INFEASIBLE`).
- **`xsa query` does not sort positions within a pattern** (§7).

---

## APPENDIX — the exact sequence used for this guide

```sh
# 0. toolchain check          cargo 1.97.1, g++ 13.3.0, python3 3.12.3, bzip2 1.0.8
cd xsa && cargo build --release                       # 4m 13s

# 1. fixture (5 pages, multistream-shaped bz2)
split -n 3 -d fixture.xml part. && for f in part.0*; do bzip2 -c "$f" > "$f.bz2"; done
cat part.0*.bz2 > fixture.ms.bz2
./xsa corpus --wikipedia fixture.ms.bz2 --out out    # docs=5 corpus_bytes=198 …

# 2. the 150 MB fixture is already a valid corpus (ends in 0x1E)
tail -c 1 /dev/shm/t150m.txt | xxd                  # 1e

# 3. build (chi absent), then with chi
./xsa build --tape --input /dev/shm/t150m.txt --scratch /dev/shm/…/s150 \
            --threads 16 --memory-gb 24
./xsa build --tape --input /dev/shm/t150m.txt --scratch /dev/shm/…/s150chi \
            --threads 16 --memory-gb 24 --with-chi

# 4. query + brute-force oracle
./xsa query /dev/shm/…/s150/t150m.sxi2 --patterns patterns.bin -j 4 > q.out
./oracle /dev/shm/t150m.txt patterns.bin > oracle.tsv
diff <(sort -n -k1,1 -k2,2 oracle.tsv) <(sort -n -k1,1 -k2,2 q.out)
```

### Document-bounded query context

`xsa query CONTAINER 'domestic cat' --docs /path/to/corpus-directory --context 120`
appends a snippet column to each normal TSV hit. Without `--context`, output is
unchanged. The directory must contain matching `seps.bin`, `titles.bin`, and
`corpus.bin`; normal sidecar discovery also works. Context is the half-open byte
window `[max(doc_start, pos-N), min(doc_content_end, pos+N))`, excluding the trailing
0x1E document delimiter. A hit on a delimiter is refused. Newlines, carriage
returns, tabs, and backslashes are escaped as `\n`, `\r`, `\t`, and `\\`; other
non-ASCII/nonprintable bytes use `\xHH`, so snippets are losslessly byte-decodable.
`--output jsonl --context N` adds a `contexts` array aligned with `offsets`, with
`start`, `end`, and the same escaped `snippet` string. JSON escaping is an outer
layer. `--limit K` works with context; count-only modes cannot request snippets.
The journal prints `QUERY_CONTEXT window=N clamped_hits=K`.

Count results are checked against the index length before emission. An impossible
result fails with `QUERY_COUNT_IMPOSSIBLE count=... pattern_len=...`. The final
m13 sample's run stride is not a byte count: rank beyond that sample's bounded
scan uses the total number of rows for the symbol.

Run the committed large-corpus count/context regression:

```sh
python3 tools/binbwt/tests/query_count_context_gate.py /path/to/xsa \
  /mnt/nvme2n1/erikg/wf-finish-as-100gb-20261102T044713Z/corpus.sxi2 \
  /mnt/nvme2n1/erikg/wiki
cargo test --release --manifest-path xsa/Cargo.toml qlite -- --nocapture
```

The gate includes the downstream binary reproducer, deterministic random and long
absent patterns, absent corpus bytes, document bounds for every context, and three
reported corpus-byte spot checks. The unit tests deliberately inject UINT64_MAX
and an interior separator to demonstrate the fail-loud assertions.

### Maximal exact matches: `xsa mem`

```sh
xsa mem corpus.sxi2 "domestic cat"              # longest MEM of the query
xsa mem corpus.sxi2 "cat sat" --all-mems        # every maximal match
xsa mem corpus.sxi2 "cat sat" --docs DIR --limit 8 --jsonl
xsa mem corpus.sxi2 --serve -j 32               # framed; see SERVING.md
```

A maximal exact match (MEM) is a substring `query[a..b)` that occurs in the corpus
and cannot be extended by the query's own neighbouring byte on either side (the
query boundary is not an extension point). `xsa mem` reports `query_start`,
`query_len`, `occurrences` (the interval width, i.e. exact overlapping occurrence
count) and up to `--limit` positions located exactly as `xsa query` locates them
(`doc_id`, `offset`, `title` when sidecars resolve). Default `--min-len 8`;
default output is the single longest MEM, `--all-mems` reports every one, `-k N`
caps that list. Empty queries are refused. Framed `--serve` mode answers the
longest MEM only, with the frame layout documented in SERVING.md.

`xsa mem` **supersedes `xsa mems` for SXI2 v6 lite containers**: `mems` goes
through the phi-era reader and fails on them with `SXI: invalid header`. Use
`xsa mem` (or `xsa mems` only for the archived legacy artifacts it was written
for).

Gates: `tools/binbwt/tests/mem_gate.py` (X1 independent mmap brute force via
`tools/binbwt/tests/mem_oracle.c`, X2 edge cases, X3 document spot checks) and
`tools/binbwt/tests/mem_bench.py` (X4 throughput of `--serve -j 32`).

### Adaptive exact-match seeding: `xsa seeds`

```sh
xsa seeds corpus.sxi2 article.txt                     # seed an article (target fan-out 50)
xsa seeds corpus.sxi2 article.txt --target-mult 500   # looser neighbourhood
xsa seeds corpus.sxi2 - --target-mult 50 --jsonl      # a fragment on stdin
xsa seeds corpus.sxi2 article.txt --max-mult 200 --min-mult 2   # multiplicity-band mode
xsa seeds corpus.sxi2 article.txt --docs DIR --limit 32 --jsonl
```

A **seed** is an exact substring of the *text you pass in* (not of the corpus: the
text is normally an article or a fragment of the pile) that is at the same time
**specific** (long) and **recurring** (multiplicity inside a band). No embeddings,
no tokenizer, no vectors: the only primitive is the FM-index interval, so seeds
are cheap, exact and deterministic. `xsa seeds` reuses the same SXI2 v6 lite
kernel as `xsa branches` and `xsa mem` and reads no member they did not need.

`--target-mult T` (default 50) is the driving parameter: the seed LENGTH is
solved for, because multiplicity is **non-increasing in length** (a longer match
can only lose occurrences). Per window start:

1. the longest occurring prefix of `text[i..i+--max-len]` is found by bisection
   on prefix occurrence (`--min-len` default 12, `--max-len` default 64);
2. the length is bisected for the longest seed with multiplicity `>= T` - the
   closest approach to the target from above - and the seed one byte longer is
   reported as the length the solve **gave up** at;
3. `--nearest` may instead emit that one-byte-shorter seed when it is closer to
   `T` in log space, and says so (`SEEDS_NEAREST`).

A window is accounted for by name: `no_match` (not one byte of it occurs),
`too_short` (longest occurring prefix below `--min-len`), `too_rare` (even at
`--min-len` the multiplicity is below `T`) or `too_common` (the longest occurring
prefix is already above the ceiling, so every admissible length is more frequent
still). `--max-mult M --min-mult m` is the alternative band mode: solve for
`m`, refuse anything above `M`.

Seeds are emitted in text order, `--stride S` (default `max-len/2`) apart,
deduplicated by content, and suppressed when wholly contained in an
already-emitted seed. `--max-seeds K` (default 64) stops the walk. Each seed
carries up to `--limit` sample positions located exactly as `xsa query` locates
them (`doc_id`, `offset_in_doc`, `title` when sidecars resolve), evenly spaced
across the interval. `SEEDS_STATS` journals windows, outcomes, mean length, the
multiplicity histogram, probe counts and scan/locate time. Empty text is refused;
`0x1E` is an ordinary byte, as everywhere else.

Gates: `tools/binbwt/tests/seeds_gate.py` with the independent occurrence oracle
`tools/binbwt/tests/seeds_oracle.c` (one mmap pass per pattern, `memmem`,
overlapping occurrences, no index code) - it re-counts every emitted seed, the
one-byte-longer seed, and the whole multiplicity ladder at each emitted offset.

### Restart after interruption

Tape builds now keep `scratch/checkpoint.journal`. Reissue the same command to
verify the corpus, remap, options, and binary hashes and replay an incomplete
build from input. A completed container is hash-verified and reused.
`--no-resume` explicitly starts clean. A mismatched plan refuses without changing
build-owned scratch files. SIGTERM/SIGINT/SIGHUP drain the current admitted wave
and merger piece, journal `BUILD_INTERRUPTED`, and exit 75. No timeout is added.

This is conservative **replay**, not incremental suffix-key resume: completed
computation is repeated and ordinary band deletion remains bounded. Read
[RESUME.md](RESUME.md) for journal semantics, signal limitations, measured gates,
and the reasons for this design deviation.
