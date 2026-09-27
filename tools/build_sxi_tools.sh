#!/usr/bin/env bash
# Build the container writer, pilot Phi extractor and SXI-aware slim consumer.
# The vendored endpoint producer is built separately; see build_rpfbwt_tap.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:?usage: bash tools/build_sxi_tools.sh OUT_DIR}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
g++ -O3 -std=c++17 -Wall -Wextra bit6/rpfbwt_endpoints.cpp -o "$OUT/rpfbwt_endpoints"
g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_write.cpp -o "$OUT/sxi_write"
g++ -O3 -std=c++17 -Wall -Wextra -fopenmp bit6/phi_inverse_heads.cpp -o "$OUT/phi_inverse_heads"
bash tools/build_slim_dump.sh "$OUT/slim_dump"
cargo build --release --manifest-path xsa/Cargo.toml
cargo build --release --manifest-path agc2flat/Cargo.toml --target-dir "$OUT/agc-target"
cp "$OUT/agc-target/release/agc2flat" "$OUT/agc2flat"
