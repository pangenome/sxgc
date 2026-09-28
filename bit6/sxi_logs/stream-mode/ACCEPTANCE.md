# Stream-mode acceptance — checked; external review pending

Implemented default AGC FIFO streaming, explicit `--materialize`, and archive-backed
sampled witness auditing. Scope: seven source/test/documentation files listed in
`changed-files.json`; pre-existing dirty work retained. `lane.diff` isolates this lane.
No tap, PFP frontend, endpoint producer, repair, or core format implementation was changed.
No commits or staged files. The running 466 tree, scratch and processes were not modified.

## Gates

- **Battery PASS:** five admitted three-record fixtures (2k/20k/200k random,
  600k perturbed duplicates, 18k satellite), plus duplicate names across two
  samples. Stream/file builds have identical six SXI members; the first five
  fixtures also compare parse and dictionary bytes. Random ranges and ranges
  crossing separators match legacy materialized bytes. See `battery-final-source.log`.
- **Full yeast235 PASS:** fresh `/tmp/sxi-stream/yeast/yeast235-stream.sxi` and
  `/tmp/sxi-stream/yeast/yeast235-control.sxi` publish; all five core members are
  compared byte for byte by `run_yeast.py`. Hashes/counts/lengths are in
  `yeast-members.json`. The requested old yeast235 path was absent, so the
  control was built fresh in this lane's scratch with `--materialize`.
- **Archive audit PASS:** both fresh yeast builds verify 32 witnesses before
  publication. A prior-artifact preflight also passes with `/does-not-exist`
  as the text argument. Corrupted range bytes fail the real audit and block
  publication. Malformed/short requests and invalid names fail loudly.
- **Lifecycle PASS:** exactly one producer invocation; byte-exact FIFO transfer;
  failed writer, failed reader, early reader exit, failed journal write, and
  SIGTERM clean up the FIFO and owned children. Default AGC scratch contains
  no `collection.txt`. See `fifo.log` and battery failure stage logs.
- **Regressions PASS:** all three committed Python regressions, both Cargo
  test commands, and 216,000 committed LCE primitive checks. See
  `regressions.jsonl`, `lce-regression.log` and individual logs.
- **Memory PASS:** sequential full builds have maximum observed process-tree
  RSS 8,852,004 KiB (9.064 GB), below 20 GB. Stage RSS/timing and
  monitor results are in `build-stages.json` and `hygiene-and-memory.json`.

## Review and limits

The audit remains sampled, not a proof of complete chi-set correctness. Its
range service retains O(record count) metadata and the AGC decoder working set;
it never materializes collection text. SIGKILL/host loss cannot run cleanup;
any leftover FIFO is confined to the unique failed scratch directory and is
never reused. The unchanged endpoint repair policy rejects an exact-duplicate
exploratory fixture in legacy file mode; final fixtures stay within admitted policy.
The full stream was already running when failure-path cleanup was hardened to
unlink the FIFO even if journaling fails. Final-source battery and lifecycle
gates were rerun afterward; streamed bytes and downstream construction were
unchanged. External reviewer approval is still required. No review agent was invoked.
The requested `contact_supervisor` tool was unavailable in this session.

Reproduction commands: `COMMANDS.md`. Structured evidence: `changed-files.json`,
`source-and-tool-hashes.json`, `protected-component-hashes.json`, stage journals,
and all retained `.time`/`.log` files under this directory.
