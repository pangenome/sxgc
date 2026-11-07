# Number provenance and evidence limits

Evidence baseline: `c46a85217b9b269c91179cbebc396f96453181af` (`pile-week-v1`).
All paths below refer to that revision unless a different full commit is given.
Read any pinned source with `git show COMMIT:FILE | nl -ba`; section names are
also supplied because external studies have their own line numbering.
No historical experiment was rerun to write this manuscript.

## Counting rule (P2)

Every quantitative claim in prose/table/abstract is wrapped in
`\qnum{claim-id}{printed value}` in `main.tex`. Each ID has one row below;
repeated occurrences must have exactly the same value. This counts distinct
quantities, not distinct digit strings (different zero-valued facts are not
inferred from one another). Two additional literal CLI parameters are listed
separately. Formula `n+1` is the documented END convention, not a measurement.
File/member names, source line numbers, commit hashes, bibliography URLs,
LaTeX layout parameters and automatically generated section/reference/page
numbers are identifiers or typesetting, not empirical quantities.

## Included quantities

| ID | Printed TeX value | Source file or primary publication | Section / scope | Command |
|---|---|---|---|---|
| input | `10^{11}` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| container | `63{,}163{,}489{,}696` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| wall | `12:35:10` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| scratchratio | `0.789` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| chi | `24{,}185{,}339{,}204` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| limit | `5` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| querywall | `0.52` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| rss | `547` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| containershort | `63` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| arena | `66{,}846{,}720` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| arenashort | `64` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| rsscap | `550` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| decoded | `0` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, query transcript and bounded reader explanation | Q |
| density | `0.242` | `tools/binbwt/RESULTS_PILE_100GB.md` | What this corrects, lines 16–22 | B |
| wikidensity | `0.156` | `tools/binbwt/RESULTS_PILE_100GB.md` | What this corrects, lines 16–22 | B |
| densitydelta | `55` | `tools/binbwt/RESULTS_PILE_100GB.md` | What this corrects, lines 16–22 | B |
| containerratio | `0.632` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| scratch | `78{,}880{,}730{,}679` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| waves | `102` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| ties | `32{,}442{,}986{,}338` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| orderrss | `108.7` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| finishrss | `32.2` | `tools/binbwt/RESULTS_PILE_100GB.md` | opening receipt, lines 1–14 | B |
| thecount | `704{,}942{,}163` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, unchanged counts | C |
| harper | `55` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, unchanged counts | C |
| functioncount | `4{,}785{,}152` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §1, unchanged counts | C |
| sample | `200{,}000` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §2 / Positions at scale | V |
| avoided | `0.295695` | `RESUME.md` | 150 MB killed mid-sweep, then restarted, lines 342–363 | R |
| ten | `10` | `RESULTS_10G_REBANK.md` | The run, lines 3–14 | T |
| tenwall | `31:12` | `RESULTS_10G_REBANK.md` | The run, lines 3–14 | T |
| haps | `466` | `RESEARCH.md` | THE 466 FROM-ZERO MILESTONE IS DONE, lines 2460–2469 | H |
| hprcn | `1{,}403{,}221{,}068{,}491` | `RESEARCH.md` | THE 466 FROM-ZERO MILESTONE IS DONE, lines 2460–2469 | H |
| hprcchi | `2{,}250{,}211{,}129` | `RESEARCH.md` | THE 466 FROM-ZERO MILESTONE IS DONE, lines 2460–2469 | H |
| catrss | `269` | `SERVING.md` | Measured: the Cat article on the 100 GB container, lines 370–382 | F |
| catwall | `49.03` | `SERVING.md` | Measured: the Cat article on the 100 GB container, lines 370–382 | F |
| ctx | `553.836` | `SERVING.md` | Measured gates, lines 120–141; tools/binbwt/evidence/branches/results.json, W2 | W |
| workers | `32` | `SERVING.md` | Measured gates, lines 120–141; tools/binbwt/evidence/branches/results.json, W2 | W |
| sentinel | `0xF5` | `RESULTS_SENTINEL_NEGATIVE.md` | Experiments and What this proves, lines 8–27 | N |
| rank | `5615` | `RESULTS_SENTINEL_NEGATIVE.md` | Experiments and What this proves, lines 8–27 | N |
| rowcut | `7.9` | `tools/binbwt/RESULTS_ROWCODE.md` | T2–T4 at e2f1a59b5a0f593c3aeb1c1f8a22d7fb48d82a5e (not present at mainline pin) | D |
| rowwall | `15.9` | `tools/binbwt/RESULTS_ROWCODE.md` | T2–T4 at e2f1a59b5a0f593c3aeb1c1f8a22d7fb48d82a5e (not present at mainline pin) | D |
| scratchcut | `3.48` | `tools/binbwt/RESULTS_ROWCODE.md` | T2–T4 at e2f1a59b5a0f593c3aeb1c1f8a22d7fb48d82a5e (not present at mainline pin) | D |
| chiram | `0.11` | `tools/binbwt/RESULTS_MEMORY_AB.md` | phase table and verdict, lines 1–18 | A |
| chidisk | `0.13` | `tools/binbwt/RESULTS_MEMORY_AB.md` | phase table and verdict, lines 1–18 | A |
| missed | `192` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §2 / 192-offset discrepancy | S |
| ycdate | `2001` | `https://aclanthology.org/J01-1001/` | primary publication metadata; bibliography entry yc | L |
| lodhidate | `2002` | `https://jmlr.org/papers/v2/lodhi02a.html` | primary publication metadata; bibliography entry lodhi | L |
| fullpile | `1.31` | `tools/binbwt/RESULTS_PILE_100GB.md` | Pile projection (1.31 TB); a target, not a completed measurement | B |

| bad | `0` | `tools/binbwt/RESULTS_PILE_100GB.md` | Search certificate §2, 192-offset VERIFY transcript | S |

## Literal command parameters and conventions

| ID | Value | Source | Meaning |
|---|---|---|---|
| build-threads | 16 | RESUME.md opening exact build command, lines 9–11 | illustrative reproduction setting, not claimed headline setting |
| build-memory | 24 | RESUME.md opening exact build command, lines 9–11 | GiB CLI setting, not measured RSS |

The verbatim command's `5` is `limit`; its `200000` is `sample` above.
A single ordered-stream pass and a single identity/histogram preparation pass
are structural claims: FUSED_TAPE.md opening, RESUME.md “One corpus pass, not
five”, and scatter_sort.cpp lines 1179–1180 and 2526–2527. They do NOT establish
a one-read corpus construction. `n+1` and virtual END are specified in
REPRODUCE.md raw-input and query-semantics sections. m1/m5 are member names.
The reported ~550 MiB is a rounded description of the 547 MiB observation,
not a proved hard cap. The 64 MB arena label is the source's approximation;
the exact allocation is 66,846,720 B. `decoded_bytes=0` is a materialisation
counter; actual cache decodes are nonzero. The final `bad=0` is separately
supported by the 192-offset VERIFY transcript.

## Commands and provenance quality

`CORPUS=/mnt/nvme2n1/erikg/pile-head100.txt` and
`CONTAINER=/mnt/nvme2n1/erikg/P100c/pile-head100.sxi2` identify the headline
input and output. Replace `XSA` with the matching built binary. Historical
absolute scratch paths are evidence locations, not guaranteed downloads.
Commands below are not claimed to have been executed during paper preparation.
An unavailable original invocation does not turn a committed result into a
fresh replication: it is a reproduction limitation explicitly recorded here.

- **B (build):** The committed headline receipt records `XSA_BUILD_DONE`, stage
  walls/RSS, scratch, waves and ties, but does **not** contain the full original
  build command. Original exact invocation: **UNVERIFIED**. The paper provides
  the documented recipe `xsa build --tape --input "$CORPUS" --scratch "$SCRATCH"
  --threads 16 --memory-gb 24 --with-chi` from RESUME.md, not the historical
  resource settings. Inspect the receipt with
  `git show c46a852:tools/binbwt/RESULTS_PILE_100GB.md | sed -n '1,26p'`.
  Ratios are reported rounded: container/n, scratch/n, chi/n;
  `(0.242/0.156-1)*100` rounds to 55. 63 GB is rounded container size.
- **Q (locate):** exact recorded query:
  `xsa query /mnt/nvme2n1/erikg/P100c/pile-head100.sxi2 --limit 5 " function "`.
  Time/RSS wrapper reproduction: `/usr/bin/time -f 'WALL %e s RSS %M kB'` before
  that command. Source reports 560140 kB, rounded to 547 MiB; logs contain
  `arena_bytes=66846720` and `decoded_bytes=0`. Shared-host observation.
- **C (counts):** `xsa query "$CONTAINER" --count "the "`,
  `xsa query "$CONTAINER" --count "Harper Reed"`, and
  `xsa query "$CONTAINER" --count " function "`. The paper's batched form
  asks these same patterns together; it is not a separate timing claim.
- **V (sample locations):** exact recorded invocation
  `xsa verify <pile> --corpus <corpus> --limit 200000 -j 200 " function "`.
  The certificate's VERIFY_TOTAL and set comparisons establish no bad or
  duplicate offsets in this sample, not exhaustive correctness.
- **S (overlap adjudication):**
  `tools/binbwt/pile_search_certificate.sh "$CONTAINER" "$CORPUS" " function " 200000`.
  This is the committed reproducible harness; its default worker count differs
  from V unless `JOBS=200` is set. It compiles `pile_pos_scan.c` and `posset.c`,
  compares the scanner against `LC_ALL=C grep -a -b -o -F`, extracts missed
  contexts, and queries them. The 192-offset receipt explicitly records
  `VERIFY pattern= function  n=192 ok=192 bad=0` and empty set differences.
- **R (restart):** reissue the same RESUME.md opening build command after an
  invocation with `XSA_FINISH_SPOOL=1 XSA_KILL_AT=after_fsync_before_unlink`.
  Source's measured-gate subsection records the fixture, receipt and identical
  container hash. Full historical wrapper/path/knob invocation: **UNVERIFIED**.
  `0.295695` is the printed summary over unlike phase work units; remap reuse
  alone accounts for it. It is not a time percentage or finish reuse claim.
- **T (10 GB rebank):** recorded parameters: `--memory-gb 100
  --finish-as-gb 400 --wit-ram-gb 8`, threads 96, default piece window.
  Input `real10b-ref/pile-10b-snap.txt`; original full launcher: **UNVERIFIED**.
  The source records finish `wall 31:12` and a byte-identical SHA. “10 GB” is
  the rung label, not a claim of an exact power-of-ten input length.
- **H (HPRC):** recorded invocation: `sealed-tools-v2 xsa build
  --agc HPRC_r2_assemblies_0.6.1.agc --threads 48 --verify-text-sample 10000`.
  RESEARCH.md's milestone pairs exact N/chi with two equal container hashes.
  This older archive route is not the Pile tape pipeline. H466_LANE_HANDOFF.md
  documents earlier incompatible input conventions, not this milestone.
- **F (Cat fanout):** source describes defaults with `--top 15`, Cat document
  extracted from the sidecars, and supplies committed `cat_q1.tsv` plus
  `cat_q1.stderr.log`. Reproduction: `xsa fanout "$WIKI_CONTAINER"
  "$CAT_TEXT" --docs "$WIKI_DOCS" --top 15` (check interface in
  SERVING.md). Original full wrapper: **UNVERIFIED**. This materialising-reader
  run has 269 GiB peak RSS and wall 0:49.03; it is not the bounded query receipt.
- **W (warm branches):** documented harness:
  `python3 tools/binbwt/tests/branches_bench.py "$XSA" "$WIKI_CONTAINER"
  "$WIKI_CORPUS" --jobs 32 --out "$OUT"`. Exact corpus/container and binary
  provenance are in `tools/binbwt/evidence/branches/results.json`; W2 supplies
  the measured contexts/s. Loading and warmup are excluded.
- **N (sentinel):** RESULTS_SENTINEL_NEGATIVE.md records release-binary tests
  on `/tmp/psi_tiny.txt` and the `+ 0xF5` treatment, compared to verified
  seed-first SA. Original exact launcher: **UNVERIFIED**. Rank 5615 is the
  first reported order mismatch, even though chi agreed.
- **D (rowcode):** `git show
  e2f1a59b5a0f593c3aeb1c1f8a22d7fb48d82a5e:tools/binbwt/RESULTS_ROWCODE.md`.
  The file records `xsa build --tape ... --with-chi`, before/after time records,
  sampled scratch and SS_ROWCODE output. Full paired invocations:
  **UNVERIFIED**. T4 names `--input /mnt/nvme2n1/erikg/wiki/corpus.bin
  --threads 16 --memory-gb 64`; it is a partial Wikipedia probe despite the
  report's “pile” wording. 7.9% = rounded `(32-29.464)/32*100` row-byte
  reduction, not total scratch. 15.9% is the rounded increase from 27:21.08
  to 31:41.06 at the separate 10 GB rung. 3.48% is total sampled scratch
  reduction from 25654067275 to 24762276203 B. Runs overlapped.
- **A (chi A/B):** RESULTS_MEMORY_AB.md records identical `--threads 96
  --memory-gb 96`, only chi on/off, and `/usr/bin/time -v` per process.
  Original full paired invocations: **UNVERIFIED**. Incremental finish RSS
  1118 MB / ~10 GB -> 0.11n; container difference 1294233512 B / ~10 GB ->
  0.13n. These are attributed differences, not whole-process totals.
- **L (literature dates):** primary publisher metadata at the linked URLs,
  checked during preparation. Dates are bibliographic, not
  benchmark outputs; no experiment command applies.

## Additional method evidence

- `tools/binbwt/CHI_STUDY.md` is **absent at the baseline**. Use the committed
  study `222ace4044cdaaf9e3ad8710d954afc5493c12bb` (sections 1–5), accessed by
  `git show 222ace4044cdaaf9e3ad8710d954afc5493c12bb:tools/binbwt/CHI_STUDY.md`.
  The paper cites that pin explicitly, and distinguishes its bitmap proposal
  from subsequent implementation in baseline CHI_BITMAP_RESULTS.md.
- RESUME_DESIGN.md begins with the implemented native protocol and retains an
  older proposed protocol below it. The paper attributes only implemented
  behavior to the native spool arm, and keeps the driver-replay limitation.
- REPRODUCE.md has historical reader and directory-limit descriptions that are
  superseded by RESULTS_PILE_100GB.md's search certificate. Use the latter for
  the bounded-reader claim. No whole-pipeline deadlock proof is asserted.

## Excluded / UNVERIFIED requested numbers

- **145 s cold first answer on a 63 GB container: UNVERIFIED.** No matching
  committed receipt in SERVING.md or the supplied result documents. Not in
  the paper. Warm throughput cannot establish this value.
- **22,768 contexts/s: UNVERIFIED at file-and-command evidence level.**
  Commit `cb4114a155323911ac3aaf8791e990db92c7244c` reports it and commit
  `88d4e0744a05359614d28db5f0ed160a677d8ef5` repeats it with a small correctness
  check. SERVING.md still records the earlier 553.836/s run. The committed
  rare-context harness does not supply the missing measured result file.
  Excluded rather than attaching the older table's workload or memory to it.
- **7.9% total scratch saving: UNVERIFIED / wrong scope.** Only the measured
  row-byte reduction is used. The available paired total scratch result is
  3.48%, and the paper includes it explicitly.
- **One physical input pass / no materialised input file: contradicted by the
  committed route.** The input is an existing file and each wave scans it.
  The manuscript uses the defensible single ordered-stream consumption claim
  and says no *additional* text copy or full suffix array is materialised.

These exclusions preserve the task's instruction not to print unsupported
numbers as results. They remain residual limitations of the supplied evidence.
