# Normalization repair acceptance — NOT ACCEPTED

Workspace `/tmp/sxgc-laneV`; date 2026-09-27. Review gate remains pending.
This is a partial implementation and a checked blocker report, not a general
padded-to-cyclic repair. Existing separator-lane edits were preserved.

## Implemented in this lane

- `rpfbwt_endpoints.cpp`: an O(r) streaming certificate permits padding
  removal only when it preserves cyclic-T rotation order. Unsafe frames
  fail before output creation. A dollar run straddling the padding boundary
  is split using the known SA=0 row. There is no SA/LF/text walk or new
  corpus read. Raw producer `.ssa` and `.ssa_t` remain unchanged.
- `slim_lce.hpp`: auto-detect 0x1e / no-newline cyclic input from the already
  loaded run alphabet. Keep one collection end and compare through both
  record separators and the corpus seam. Legacy newline semantics remain.
- Expanded independent oracle fixtures cover plain, unseparated, legacy
  newline, repetitive, HOR-nested, multi-string, and both periodic and
  aperiodic seam-adversarial input. Endpoint checks include packed tails.
  Test-only dense simulations demonstrate the unbounded row-repair cascade.
- CLI regression accepts the *specific* earlier seam rejection while still
  requiring no publication; arbitrary endpoint errors do not pass the test.

`changed-files.json` and `lane.diff` isolate this lane from the dirty baseline.
The producer/tap, pipeline, parser, xsa front end, writer and protected
`/mnt/nvme3n1/erikg/sxgc-laneV/xsa-build-*` directories were not changed or
accessed for this lane's build. Fresh large scratch is elsewhere.

## Checked gates

- (a) **FAIL**: final separator suite has 45 checks, 30 passing and 15 failing.
  Four oracle fixture classes still need the general cyclic transformation;
  their rejection is not counted as success. The tiny AGC certified frame
  passes endpoints and slim, then the writer rejects an out-of-range chi
  witness. No 0x1e artifact was published by these gates.
- (b) **PASS for the certified single-string case**: yeast was rebuilt from
  `yeast_pfp2.txt` through fresh parsing, producer, certified adapter, slim,
  sweep, writer and container validation. **Chi remains 85,404,240; all five
  embedded members are unchanged**, with identical SHA-256, sizes, counts
  and CRCs. No `yeast_v2.sxi` is needed. The audit rebuild is published at
  `/mnt/nvme3n1/erikg/sxi-normalization-repair/yeast-certified.sxi`.
  `yeast-members.json` records the complete comparison against published
  `/mnt/nvme3n1/erikg/sxgc-laneV/yeast.sxi`. This does not certify the
  adapter for general seam-repair cases.
- (c) **FAIL**: full `yeast235.agc --agc` build completed preparation and
  both parse stages and rpfbwt, then the seam certificate rejected the frame
  with 6,893 terminal predecessors before SA=0. Neither normalized endpoints
  nor an SXI was written; there is no frame for the requested control gate.
  Materialized T has 3,336,986,759 bytes and 9,901 records. See
  `yeast235-gate.json`, `yeast235-build.log`, and the per-stage journal.
- (d) **PASS**: format regression and final CLI regression pass. The CLI
  assertion was updated to recognize the new fail-closed rejection stage.

Additional evidence:

- 1,022 binary inputs: 252 certified outputs exactly match cyclic BWT,
  run heads and tails; 770 safely rejected; zero invalid output frames.
- Direct combinatorial certificate audit: 29,523 ternary texts, 10,125
  certified, zero false certificates.
- Direct cyclic-LCE oracle: 1,295,116 comparisons across the original
  multi-record fixture, periodic and near-periodic input, and input without
  a separator; all pass, including equality through the seam.
- Producer `.ssa` and `.ssa_t` still match the independent padded oracle
  on the original 2,003-byte failing fixture.
- Full tiny AGC witness evidence: n=20,001, chi count=13,560, one out-of-range
  witness=20,002. See `chi-range-blocker.json` and final stage logs.

## Mathematical verdict and remaining work

`DERIVATION.md` documents the sufficient certificate, source investigation,
and the actual obstruction. For T=A^m B A^m (even m), the raw padded BWT has
six runs while m rotation rows change and a backwards row repair needs m-1
relocations. Consequently no O(w)-sized or row-by-row r-bounded seam repair
is justified. This does **not** prove a compressed-interval r-space transform
impossible. That algorithm remains to be derived and implemented.

The installed parser requires window >=3 and unconditionally pads by w.
The producer's `suffix_length` is an internal dictionary suffix length;
its circular adjustment is a coordinate conversion. No supported no-pad
or pad-transparent invocation was found. No producer usage was changed.

Separately, chi-rspace assumes N=n+1 and suppresses newline sentinel class 0.
Ordinary 0x1e can produce a witness n+1 from SA=0, outside the writer's accepted
range. This needs a cyclic-coordinate derivation, not a sentinel relabel or
unchecked modulo patch. Front-end code was not changed.

No `contact_supervisor` tool was available in the tool catalog, so no decision
or review approval is claimed. The required independent reviewer must see
these blockers; no final acceptance is asserted. No commits or staged files.

## Final resource and hygiene evidence

Maximum recorded process RSS: 8,820,756 KiB. Maximum observed
combined process-tree RSS: 17,585,352 KiB (18.007 GB), below 20 GB.
Yeast used a 19,000,000 KiB address-space cap; concurrent AGC used
9,700,000 KiB. See per-stage `.time` files, `observed-rss.jsonl`, and
`hygiene-and-memory.json`. Diff hygiene passes; staged-file list is empty.
