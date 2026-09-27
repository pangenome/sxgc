#!/usr/bin/env bash
# Copy upstream, apply only the additive endpoint tap, build with upstream CMake.
set -euo pipefail
cd "$(dirname "$0")/.."
SOURCE="${1:-/home/erikg/r-pfbwt}"
DEST="${2:-/tmp/rpfbwt-sxgc}"
if [[ -e "$DEST" ]]; then
  echo "refusing existing destination: $DEST" >&2
  exit 1
fi
cp -a "$SOURCE" "$DEST"
patch -d "$DEST" -p1 < bit6/patches/rpfbwt_emit_tails.patch
FLAGS=()
# Reuse the copied dependency sources, never their upstream build outputs.
for DEP in "$DEST"/build/_deps/*-src; do
  [[ -d "$DEP" ]] || continue
  NAME="$(basename "$DEP" -src)"
  FLAGS+=("-DFETCHCONTENT_SOURCE_DIR_${NAME^^}=$DEP")
done
cmake -S "$DEST" -B "$DEST/build-sxgc" -DCMAKE_BUILD_TYPE=Release "${FLAGS[@]}"
cmake --build "$DEST/build-sxgc" -j "${BUILD_JOBS:-8}"
echo "$DEST/build-sxgc/rpfbwt"
