# Reproduction

Run from `/tmp/sxgc-laneV`. Retained prerequisites are the existing query-product
writer and baseline binary, certified original battery containers/text, and
yeast235 container/text. No protected k10 path is accessed.

```sh
# The saved baseline is the pre-change release executable.
# Do this BEFORE rebuilding when repeating the lane from its baseline sources:
cp xsa/target/release/xsa /tmp/sxi-rope-native-baseline
cargo build --release --manifest-path xsa/Cargo.toml

make -C /tmp/pi-github-repos/runtime-iM4bba/c73bf005e0630ea7936d5e6556e9d9dd3902b92cd1a35a5b3ffece651d8f6c29 -j2

python3 bit6/test_rope_compat.py --battery \
  --work bit6/sxi_logs/rope-compat/fixtures \
  --rope /tmp/pi-github-repos/runtime-iM4bba/c73bf005e0630ea7936d5e6556e9d9dd3902b92cd1a35a5b3ffece651d8f6c29/ropebwt3
python3 bit6/test_rope_yeast.py
python3 bit6/sxi_logs/rope-compat/run_regressions.py
bash tools/build_slim_dump.sh /tmp/sxi-rope-lce-test tools/slim_lce_test.cpp
/tmp/sxi-rope-lce-test
rustfmt --check xsa/src/product.rs
git diff --check
git diff --cached --name-only
```

The Python runners impose RLIMIT_AS=9 GiB on themselves and children. The
LCE compilation/test were also run with that limit. Every fixture command
and its exit code is retained in `fixtures/commands.jsonl`, with stdout,
stderr, and `/usr/bin/time -v` files beside it. Yeast has the same layout in
`yeast/`; the existing regressions are listed in `regressions.jsonl`.

The real tool is built from the cloned MIT source. Its exact commit, source
and binary hashes, and license text are in `ropebwt3-provenance.json`.
`ropebwt3-build.log` records compiler output.

`baseline/product.rs` and `baseline/SXI_QUERY.md` preserve the inherited
query lane. `product.diff` and `docs.diff` isolate this lane's modifications.
`inherited.diff` is the tracked dirty diff before work; `hygiene.json`
checks it remains byte-identical after work. New tests and evidence are
untracked additions; no commits or staging were performed.
