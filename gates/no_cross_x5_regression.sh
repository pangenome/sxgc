#!/usr/bin/env bash
# X5 - NO REGRESSION: the separator corpus path (pile.txt) unchanged.
#
#   1. the plain 150 MB pile.txt build is BYTE-IDENTICAL to the banked
#      adadf48-era sha (871c904f...) - no flag, --no-trie
#   2. the --docs-map --no-trie arm is byte-identical to the banked
#      m15-embedded container (dd56e422...) - the map embed is unchanged
#   3. chi is unchanged at 150 MB and asserted at 10 GB by X2
#   4. cargo clean + full release build + cargo test
#
# usage: gates/no_cross_x5_regression.sh WORKDIR
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
D=${1:?workdir}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/sd-scratch/pile150.txt}
MAP=${MAP:-/mnt/nvme2n1/erikg/pilemap_full}
PLAIN_SHA=871c904f3a67856943a652d1bad743b826cf6892e2265852d9b4f619ff1f98d5
MAP_SHA=dd56e422811c55374ded1a4c703aeff0219427be86a90e55bb371f0ecec99faa

fail=0
say() { echo "X5_$*"; }
bad() { echo "X5_FAIL $*"; fail=1; }

mkdir -p "$D"
echo "X5_BOX $(uptime)"

build() { # dir flags...
  local dir=$1; shift
  rm -rf "$dir"
  mkdir -p "$dir"
  "$X" build --tape --input "$CORPUS" --scratch "$dir" --threads 24 \
     --memory-gb 24 "$@" > "$dir.log" 2>&1
  local rc=$?
  [ $rc -eq 0 ] || { bad "build $* exited $rc (see $dir.log)"; return 1; }
  sha256sum "$dir/$(basename "${CORPUS%.txt}").sxi2"
}

echo "== the plain build (no map, no trie): byte-identity with adadf48"
S=$(build "$D/plain" --no-trie) || exit 1
say "PLAIN $S"
case "$S" in
  "$PLAIN_SHA"*) say "PLAIN_IDENTITY exact" ;;
  *) bad "the plain build drifted: $S" ;;
esac

echo "== the --docs-map --no-trie build: byte-identity with the m15-embedded banked container"
S=$(build "$D/map" --no-trie --docs-map "$MAP") || exit 1
say "MAP $S"
case "$S" in
  "$MAP_SHA"*) say "MAP_IDENTITY exact" ;;
  *) bad "the map build drifted: $S" ;;
esac

echo "== chi at 150 MB (the journal's own assertion)"
grep -E "CHI_ASSERT|chi_count" "$D/plain.log" | tail -1

echo "== cargo clean + full release build + tests"
( cd "$ROOT/xsa" && cargo clean && cargo build --release > "$D/cargo-build.log" 2>&1 )
[ $? -eq 0 ] || { bad "cargo build failed (see $D/cargo-build.log)"; tail -20 "$D/cargo-build.log"; }
( cd "$ROOT/xsa" && cargo test --release > "$D/cargo-test.log" 2>&1 )
if [ $? -eq 0 ]; then
  grep -E "test result" "$D/cargo-test.log" | tail -1 | sed 's/^/X5_CARGO_TEST /'
else
  bad "cargo test failed (see $D/cargo-test.log)"
  tail -20 "$D/cargo-test.log"
fi

say "RESULT $([ $fail -eq 0 ] && echo PASS || echo FAIL)"
exit $fail
