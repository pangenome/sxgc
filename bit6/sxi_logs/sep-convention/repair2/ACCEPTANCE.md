# Seam repair acceptance — checked; external review pending

Workspace `/tmp/sxgc-laneV`; 2026-09-27. Mandatory gates (a), (b), and (c) pass. Optional k10 gate (d) was not run. No fresh k10 chi or boundary delta is claimed.

## Implemented scope

- The adapter retains the certified streaming path and repairs admitted seam classes with compressed prefix intervals, Phi samples and exact cyclic-LCE comparisons. It recomputes runs and endpoint samples after splicing. Producer samples remain read-only.
- Class size, total class rows and discovery depth are capped by `max(1000, raw_r/1000)`. Large classes refuse loudly before either output is created. No expanded-text fallback exists.
- Adapter LCE probes and exact fingerprint verification are capped by `max(1000, bit_width(raw_n)^3)`, giving polylogarithmic admitted comparison work. The existing downstream slim default is unchanged. See `DERIVATION.md` for the exact complexity and compressed PFP preprocessing costs.
- Cyclic sweep witnesses use `(n-p)%n`, derived from the zero-based reverse coordinate of the preceding BWT character. Legacy newline-only witnesses retain their previous convention.
- Tap, producer, source front end, pipeline, writer and protected k10 work directories were not changed by this lane. Prior dirty work is preserved; `lane.diff` and `baseline/` isolate this lane.

## Gates and publications

- **(a) PASS:** all 47 separator checks pass (the original 45 plus two explicit refusal checks). All 15 prior failures are resolved. Original small seam-adversarial cases match the independent cyclic oracle; the new constant-run adversary refuses as designed.
- **Additional oracle PASS:** 2,214 endpoint cases (1,436 repaired; zero mismatches), 74 dense aggregate/witness checks across both sweep modes, including periodic ties and cyclic newline/0x1e mixtures. A six-run frame representing 2,000,000,001 bytes refuses `class_size=2000000000 limit=1000` from only 976 encoded RLE bytes, with no text, dictionary, parse or output files.
- **(b) PASS:** fresh yeast build publishes `/mnt/nvme3n1/erikg/sxi-repair2/yeast.sxi`. Chi is **85,404,240**. All five core members retain identical SHA-256, codec, bytes, count and CRC versus `/mnt/nvme3n1/erikg/sxgc-laneV/yeast.sxi`; dimensions are unchanged. See `yeast-members.json`.
- **(c) PASS:** full fresh `--agc /home/erikg/yeast/yeast235.agc` publishes `/tmp/sxi-repair2/yeast235.sxi`. **Measured chi: 85,404,336**. n=3,336,986,759; k=1; r=100,905,045. The terminal-predecessor certificate count is 6,893; actual repair covers seven maximal classes and 13,503 candidate rows, found in eight extensions. Limit=100,905; max verified parse prefix=22,263, below the 32,768 comparison-work cap.
- The independent fresh text-control build publishes `/tmp/sxi-repair2/yeast235-control.sxi`, passing exact head/RI4 byte gates and matching all five AGC core members, dimensions and chi. Both builds ran fresh preparation/parse/producer stages as applicable; only the explicitly labeled adapter preflight reused earlier AGC artifacts. Large control equality is an internal-consistency gate; independent cyclic order is established on the bounded oracle fixtures and by the class proof.
- The unchanged writer checks endpoint/range/uniqueness consistency and checksums; `xsa sxi-info` validates each candidate before no-clobber publication.
- **(d) NOT RUN:** optional approximately 4.6-hour k10 gate was not launched. The existing `h10_rl.txt` still ends in `0x0a`; no canonical-0x1e k10 artifact, measured chi or boundary-delta claim is supplied. See `k10-status.json`.
- Final CLI and adversarial format regressions pass. The CLI assertion was updated to recognize only the specific bounded-repair refusal, while still requiring no publication.

## Published yeast235 member hashes

| Member | Bytes | Count | SHA-256 |
|---|---:|---:|---|
| 1: runs | 504527273 | 100905045 | `663a45ab36f5b90e95e77bc1d652f9066777183c559e6620fe2a74ad1bec9b07` |
| 2: packed tails | 403620193 | 100905045 | `851d352d51fb073900f1f56c6d54da0783606643e377db774c6e05d91fa8f51e` |
| 3: heads | 807240360 | 100905045 | `d73ff6b02f2c73051264f13c8ec02f4839b52e632cb54f0df840f38b2aeb203e` |
| 4: anchors | 12 | 0 | `0da64314dec135e73e86cba36a52479f4664f4b2b93ae87c9ff1834e9456298e` |
| 5: chi | 88747090 | 85404336 | `dd454f133287ff25bd5399c93d70d8310699e23c1c897485a151db8f28a75945` |
| 6: names | 351876 | 351876 | `1243e4f51957ac4ac2a4aa80642b9ac9367e5864d979f2040f26e2519278079b` |

Full metadata and the control comparison are in `yeast235-members.json`.

## Measured stages

Three builds overlapped, each with 16 requested threads. Values are **wall seconds / peak RSS KiB** for that process stage, from retained `/usr/bin/time -v` output and pipeline journals.

| Stage | Yeast regression | Yeast235 AGC | Yeast235 text control |
|---|---:|---:|---:|
| prepare-agc | — | 41.419 / 293,664 | — |
| parse | 78.294 / 951,452 | 86.094 / 948,572 | 77.048 / 953,300 |
| parse-l2 | 3.949 / 261,952 | 3.852 / 261,868 | 3.937 / 263,220 |
| rpfbwt | 554.017 / 8,824,188 | 548.261 / 8,828,908 | 566.049 / 8,825,064 |
| endpoints | 41.468 / 2,048 | 99.598 / 7,108,000 | 106.295 / 7,107,512 |
| gate-heads | — | — | 0.790 / 0 |
| gate-runs-tails | — | — | 0.880 / 0 |
| slim | 878.333 / 3,625,448 | 729.581 / 3,608,768 | 713.189 / 3,595,260 |
| sweep | 13.498 / 6,144 | 14.964 / 4,096 | 11.090 / 6,144 |
| write | 26.371 / 669,696 | 31.768 / 669,696 | 24.857 / 669,696 |
| validate | 8.541 / 2,048 | 9.772 / 2,048 | 8.363 / 2,048 |

Largest recorded process peak: **8,828,908 KiB**. Observed concurrent process-tree peak: **26,444,472 KiB (27.079 GB)**, below 150 GB. `observed-rss.jsonl`, `hygiene-and-memory.json`, per-stage `.time` files and `build-stages.json` retain the measurements.

## Review evidence and limits

- Reproduction: `COMMANDS.md`; exact stage argv and outcomes: build journals and `build-stages.json`.
- Changes: `changed-files.json`, `lane.diff`, `baseline/`, and `source-and-binary-hashes.json`. Untouched component fingerprints are in `component-hashes.json`.
- Tests: `gates/separator-gates.json`, `exhaustive-final.log`, `cli-final.log`, `format.log`, and `publication-audit.log`.
- The policy is intentionally conservative: discovery, total-class and LCE-work limits can refuse valid frames whose individual classes are small. Refusal is explicit; there is no fallback.
- `git diff --check` passes. No staged files, commits, or deletion of existing parse artifacts. Protected k10 build directories were not accessed.
- Self-review found no blockers in the implemented scope. **The required independent reviewer gate remains pending.** No `contact_supervisor` tool was exposed; no supervisor approval or external review is claimed. See `REVIEW.md`.
