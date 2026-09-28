# Distribution hardening acceptance evidence

Final tool prefix: `/tmp/sxgc-dist-final`.
Fresh checkout: `/tmp/sxgc-distribution-checkout` (local clone plus the uncommitted implementation source overlay; no commits created).
Reference manifest: `tools/MANIFEST.sha256`.
Manifest SHA256: `d766417982f137fe4d177611061c9c50a9f5d46e17cd3003b6f79c214275c84d`.
The manifest covers 8 pipeline binaries, 18 upstream commit pins, and 49 source/patch/build inputs.

## Checked gates

- Fresh-checkout `BUILD_JOBS=4 bash tools/build_all.sh /tmp/sxgc-dist-final`: PASS; see `build-all-final.log`. All dependency sources downloaded into the new prefix. Native C++ and Rust release executables were built there.
- Repeat `bash tools/build_all.sh /tmp/sxgc-dist-final`: PASS verification-only; see `idempotence.log`.
- Reference manifest verification: PASS; see `reference-manifest.log`.
- Eight manifest regressions: PASS; see `manifest-tests.log`. Covers all eight changed tool bytes, overrides, changed pin/source, malformed/incomplete manifests, missing unused tools, strict preflight before scratch, and journaled debug bypass.
- Real-tool publication plus one-byte corruption gate: PASS; see `final/distribution-gate.json`, `final/battery.log`, and `final/one-byte-drift.log`. The unchanged manifest line for `sxi_text_audit` is printed with line number and actual hash. No scratch, log directory, or SXI was created on rejection.
- All three committed Python regressions: PASS (`build-regression.log`, `format-regression.log`, `phi-regression.log`). The intentionally substituted /bin/false stage uses the explicit debug escape.
- Full source battery: PASS 7/7 with exact endpoint and aggregate byte equality; `full-battery/results.json` contains every SXI path, chi, scratch, and journal.
- Each of the seven published battery journals has exact manifest text/digest, all tool hashes, and a final PASS row binding the same manifest to the output SHA256.
- `cargo test --locked --manifest-path xsa/Cargo.toml --target-dir /tmp/sxgc-distribution-test-target`: PASS compilation, with zero Rust unit tests defined; `cargo-tests.log`.
- `git diff --check`: PASS.
- Yeast235 native-AGC gate: PASS publication, chi=85,404,336, 32/32 direct source audit samples, all 9 stages successful, no materialized collection text. Total timed stages: 1,179.01 seconds; maximum stage RSS: 8,821,084 KiB. See `yeast-gate.json`, `yeast-build.log`, and `yeast/`. The published SXI is `/tmp/sxgc-distribution-yeast/yeast235.sxi`; its SHA256 is `764f3dc7cb589223e0935d5bf0177c64f67835ab8857cb2408bdefbf776e5e11` and matches its journal.

Reproduce the small/committed gates with `python3 bit6/sxi_logs/distribution/run_regressions.py`; the exact commands/results are in `regressions.json`. Reproduce the full battery with `python3 bit6/sxi_logs/distribution/run_battery.py` (fresh output directories are allocated each run).

## Build discoveries resolved

The old build depended on machine-local PFP-DS/SDSL/TeraTools headers and a pre-existing gsacak object. The unified build downloads pinned sources and creates its own libraries/object. The first exploratory build exposed missing HTSlib; HTSlib and its pinned submodule are now built in the prefix too. `build-all.log`, `build-all-02.log`, and `build-02-continue.log` retain exploratory failures and repair evidence. The final clean build succeeds without those machine-local installations.

## Review boundaries

No algorithm changes, no n-space walks, no process signals, no staging, and no commits. Existing unrelated workspace edits were retained. Protected executable hashes were captured before work in `protected-before.json` and after work in `protected-after.json`: all 336 match exactly, with no added or removed executables (`protected-comparison.json`). `git diff --cached --quiet` confirms no staged files (`no-staged-files.txt`).

Binary digests depend on compiler/platform/build paths. Each newly built prefix gets a sealed local manifest; the checked-in manifest records this verified reference build. Existing prefixes are never silently re-blessed. Keep source checkout and prefix immutable during use. These checks prevent accidental drift, not deliberate replacement of both binaries and manifest. The launcher still needs its source checkout (or an explicit XSA_PIPELINE pointing to that matching checkout); this is not a standalone relocatable package.

Self-review: no known blockers. External reviewer approval is required and has not been claimed. The requested contact_supervisor tool was not available in this session.
