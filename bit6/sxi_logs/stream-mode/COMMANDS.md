# Stream-mode reproduction

All paths below belong to this lane. Existing dirty changes were retained in
`baseline/`; `lane.diff` isolates this lane's source changes. No commits or staging.

## Isolated builds

```sh
cargo build --release --manifest-path agc2flat/Cargo.toml --target-dir /tmp/sxi-stream/agc-target
cargo build --release --manifest-path xsa/Cargo.toml --target-dir /tmp/sxi-stream/xsa-target
cp /tmp/sxi-stream/agc-target/release/agc2flat /tmp/sxi-stream/tools/agc2flat
g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_text_audit.cpp -o /tmp/sxi-stream/tools/sxi_text_audit
```

The unchanged endpoints, slim_dump and writer tools were copied from
`/tmp/sxi-query-tools/` to `/tmp/sxi-stream/tools/`. These resolve to the prior
repaired separator implementation. The unchanged external PFP and tap producer
are `/home/erikg/pfp/build/pfp++` and `/tmp/rpfbwt-sxgc/build-sxgc/rpfbwt`.

## Gates

```sh
python3 bit6/test_sxi_stream.py --work /tmp/sxi-stream/battery-final-source \
  --xsa /tmp/sxi-stream/xsa-target/release/xsa --tools /tmp/sxi-stream/tools \
  --log-dir bit6/sxi_logs/stream-mode
python3 bit6/test_sxi_fifo.py
python3 bit6/sxi_logs/stream-mode/run_regressions.py
bash tools/build_slim_dump.sh /tmp/sxi-stream/tools/slim_lce_test tools/slim_lce_test.cpp
/tmp/sxi-stream/tools/slim_lce_test
python3 bit6/sxi_logs/stream-mode/run_yeast.py
```

Use new scratch/output paths to rerun without overwriting any publications.
The yeast runner uses a hard 18,000,000,000-byte address-space limit and
monitors its own process tree RSS once per second. Stream and file control
are sequential. The battery has the same limit; regressions use 4 GB.
All stage argv, exits, timing and RSS are retained in JSONL and `.time` logs.
The full yeast audit runs before publication and uses 32 archive witnesses.

`archive-audit-preflight.log` separately verifies 32 witnesses from the prior
yeast artifact with the literal nonexistent text path `/does-not-exist`.
It is additional range-service evidence, not a substitute for the fresh gate.

The initial exact-three-copy fixture was refused by the unchanged endpoint
repair policy even in legacy file mode; see `battery.log` and its endpoint log.
The final fixture changes two leading bases in each duplicated record to stay
within the admitted policy. Final tests do not bypass any construction gates.
