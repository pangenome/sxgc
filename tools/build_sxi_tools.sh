#!/usr/bin/env bash
# Build the container writer, pilot Phi extractor and SXI-aware slim consumer.
# These tools alone do not implement fresh source endpoint construction.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:?usage: bash tools/build_sxi_tools.sh OUT_DIR}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_write.cpp -o "$OUT/sxi_write"
g++ -O3 -std=c++17 -Wall -Wextra -fopenmp bit6/phi_inverse_heads.cpp -o "$OUT/phi_inverse_heads"
bash tools/build_slim_dump.sh "$OUT/slim_dump"
cargo build --release --manifest-path xsa/Cargo.toml
