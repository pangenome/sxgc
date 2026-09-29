# Parsed-space lane results

The three dictionary-memory changes are implemented as an opt-in experimental toolchain. The full-pile acceptance contract is **not satisfied**: gates b–d and independent review remain outstanding, and the current full-pile projection exceeds capacity. No slice is claimed as a substitute for the full pile.

## Gates

- **(a) Yeast235: PASS.** Final bounded parser outputs are byte-identical to both the accepted retained inputs and the exact inputs used by the external frontend. The frontend compares `.rlebwt`, `.rlebwt.meta`, `.ssa`, and `.ssa_t` against `/tmp/rpfbwt-64-yeast/parse`. Every measured yeast stage runs under RLIMIT_AS=3,500,000,000 bytes.
- **(b) K10: NOT RUN.** Its retained parse and the supervisor-owned memory-gate directories were not modified. The historical retained prefix has no tail reference; the recipe records PARTIAL until all four output references are available.
- **(c) Pile fragment: NOT RUN end to end.** The requested 1,082,130,213-byte raw input was copied into the worktree. The continuation fixture reserves a unique terminal (`fixtures/pile-terminal-cleaning.json`). A 100 MB prefix of the earlier cleaned fixture (`fixtures/pile-cleaning.json`) was used only for parser timing. No web chi/n is claimed.
- **(d) 10–50 GB: NOT RUN.** Ordered, no-clobber recipes are in `RECIPES.sh`, including a 10 GB prepared measurement slice. Only run after a–c pass and after checking the downstream run-ID limit.
- **Small complete regression: PASS.** The 200,001-byte cyclic fixture traversed parse → external front end → endpoints → slim → sweep → 1,000-witness audit → `.sxi` → validation. chi=136,189, chi/n=0.6809415953. This synthetic DNA fixture is not a web-text datum.

## Per-stage measurements

GNU time wall/RSS below; disk is cumulative allocated construction storage in that experiment’s work and scratch directories, sampled every 0.5 s (plus a final sample). Sampling can miss short transient peaks. Input files, compiler outputs and read-only references are excluded. The final L2 parser has time/RSS evidence but no separately polled disk peak; the earlier identical-input L2 run measured 789,766,144 bytes (`yeast-agc/journal.jsonl`).

| Case | Stage | Wall s | Peak RSS MiB | Sampled disk MiB | Address cap GB |
|---|---|---:|---:|---:|---:|
| yeast-parser-checked | parse | 489.22 | 123.70 | 1510.39 | 3.5 |
| yeast-parser-checked | parse-l2 | 5.89 | 255.76 | not sampled | 3.5 |
| yeast-final | frontend | 1925.81 | 2195.55 | 7397.89 | 3.5 |
| web-parser-bounded | parse | 22.91 | 26.00 | 290.79 | 0.5 |
| cyclic-checked | parse | 0.03 | 6.00 | 0.23 | 0.5 |
| cyclic-checked | parse-l2 | 0.00 | 4.00 | 0.26 | 0.5 |
| cyclic-checked | frontend | 8.98 | 10.00 | 4.60 | 0.5 |
| cyclic-checked | endpoints | 0.07 | 2.00 | 5.35 | 0.5 |
| cyclic-checked | slim | 0.12 | 8.00 | 9.94 | 0.5 |
| cyclic-checked | sweep | 0.02 | 6.00 | 10.98 | 0.5 |
| cyclic-checked | audit | 0.06 | 6.00 | 10.99 | 0.5 |
| cyclic-checked | write | 0.03 | 4.00 | 13.32 | 0.5 |
| cyclic-checked | validate | 0.01 | 2.00 | 13.33 | 0.5 |

## Correctness evidence

- `dictionary-test-checked.log`: four randomized byte dictionaries, every SA and LCP entry over three replay passes, rank/select membership checks, and a 70,000-byte repeated-prefix fixture checking all SA entries plus sampled exact LCPs beyond the 16-bit hint limit.
- `phrase-store-test.log`: 100,000 distinct phrases plus 100,000 duplicates, all lexical ranks/emissions, disk-chain rehashing, and a forced hash collision rejected.
- `frontend-final-test.log`: 16 byte comparisons against the pinned accepted tools across DNA/ASCII fixtures with 32 KiB SA blocks and a 64 KiB dictionary cache; unsupported properties and oversize phrases refused; owned scratch cleaned.
- `measurement-test.log`: all 256 byte values exercise the measurement cleaner, terminal uniqueness, raw-copy identity, manifest hash and overwrite refusal.
- `cyclic-checked/audit.log`: TEXT_SAMPLE_PASS requested=1000 verified=1000; writer and reader validation also passed.
- `provenance.json`, stage journals and build logs retain executable hashes, argv and build dependencies. `git diff --check`, Python syntax checks and recipe shell syntax checks passed. No commit or staging operation was performed.

## Limits and continuation

`ARCHITECTURE.md` documents all three changes, dictionary-access audit, width limits and residency assumptions. `BUDGET.md` and `full-pile-budget.json` give the measured disk placement and the full-pile projection. The endpoint tap and file formats are unchanged; the experimental merge enforces one thread/one chunk. The product pipeline has not been switched to this experimental toolchain. The current phrase is capped by `SXI_MAX_PHRASE_BYTES` (64 MiB default); the SA block budget must accommodate complete padded phrases.

The requested `contact_supervisor` tool was not available in the session catalog. No live long-running gate is handed off via this report. Rebuild and launch later gates from a durable checkout using the supplied recipes; do not launch work that will outlive an automatically reaped worktree.

Exploratory logs are preserved, but are not counted as passing gates: the first yeast attempt used a different newline input than the accepted AGC reference; three frontend attempts were explicitly stopped on this lane’s own PIDs while improving buffering, LCP reuse and boundary lookup reuse. `scratch-cleanup.json` records removal of only those owned stopped-job scratch directories.

Independent reviewer approval is still required.
