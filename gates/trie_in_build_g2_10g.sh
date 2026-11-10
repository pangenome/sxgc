#!/usr/bin/env bash
# T2/T3/T4 at 10 GB: the in-build trie vs the post-hoc trie, and what it costs.
#
#   arm A  --tape (trie ON, defaults depth 8 / min_support 1000)
#   arm B  --tape --no-trie (the product-unchanged arm: same container bytes)
#   equivalence: the m20 member of arm A against the post-hoc m20 built by
#                `xsa ctx-build` from the SAME container (member sha with the
#                provenance field zeroed; differing bytes must lie inside it)
#
# usage: gates/trie_in_build_g2_10g.sh
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
D=${D:-/mnt/nvme2n1/erikg/trie-in-build/g10}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pf-10gb.txt}
CHI=${CHI:-2676925006}
THREADS=${THREADS:-48}
MEMGB=${MEMGB:-24}
fail=0
say() { echo "G2_$*"; }
bad() { echo "G2_FAIL $*"; fail=1; }

mkdir -p "$D"
echo "G2_BOX $(uptime)"
echo "G2_CORPUS $(ls -l "$CORPUS")"
"$X" ctx-tap-selftest > "$D/tap-selftest.log" 2>&1 && say "TAP_SELFTEST PASS" || bad "tap selftest"

build() { # $1 = dir, $2 = log, $3... = flags
  local dir=$1 log=$2; shift 2
  rm -rf "$dir"
  echo "G2_BUILD_START dir=$dir flags=$* $(date -u +%H:%M:%S)"
  /usr/bin/time -f "G2_WALL %e s RSS %M kB" \
    "$X" build --tape --input "$CORPUS" --scratch "$dir" --threads "$THREADS" \
       --memory-gb "$MEMGB" --chi-expect "$CHI" "$@" > "$log" 2>&1
  local rc=$?
  grep -E "^G2_WALL|XSA_BUILD_DONE" "$log"
  echo "G2_BUILD_END dir=$dir rc=$rc $(date -u +%H:%M:%S)"
  return $rc
}

echo "== arm A: trie ON (depth 8 / min_support 1000)"
build "$D/trie" "$D/trie.log" || bad "arm A build failed (see $D/trie.log)"
grep -E "CONTEXT_TRIE_(POLICY|DEPTH|COLUMNS|TAP|BUDGET|JOIN|PUBLISHED|BUILD_WALL)" "$D/trie.log"
A=$D/trie/pf-10gb.sxi2

echo "== arm B: --no-trie"
build "$D/notrie" "$D/notrie.log" --no-trie || bad "arm B build failed (see $D/notrie.log)"
B=$D/notrie/pf-10gb.sxi2

say "ARM_A $(sha256sum "$A")"
say "ARM_B $(sha256sum "$B")"
say "SIZE_A $(stat -c%s "$A") SIZE_B $(stat -c%s "$B")"

# ---- T2: equivalence against the post-hoc member of the SAME container
REF_CONTAINER=${REF_CONTAINER:-/mnt/nvme2n1/erikg/ctx-trie/g10-d8.sxi2}
if [ -f "$REF_CONTAINER" ]; then
  "$X" trie-member "$A" -o "$D/m20-inbuild.bin" | tee "$D/m20-inbuild.txt"
  if "$X" trie-member "$REF_CONTAINER" -o "$D/m20-posthoc.bin" > "$D/m20-posthoc.txt" 2>"$D/m20-posthoc.err"; then
    cat "$D/m20-posthoc.txt"
    a=$(sed 's/.*sha256_noprov=\([0-9a-f]*\).*/\1/' "$D/m20-inbuild.txt")
    b=$(sed 's/.*sha256_noprov=\([0-9a-f]*\).*/\1/' "$D/m20-posthoc.txt")
    say "EQUIVALENCE in_build=$a post_hoc=$b ref=$REF_CONTAINER"
    [ "$a" = "$b" ] && say "EQUIVALENCE_IDENTITY PASS" || bad "EQUIVALENCE: member payloads differ"
    nd=$(cmp -l "$D/m20-inbuild.bin" "$D/m20-posthoc.bin" 2>/dev/null | wc -l)
    oof=$(cmp -l "$D/m20-inbuild.bin" "$D/m20-posthoc.bin" 2>/dev/null | awk '{o=$1} o<153 || o>168' | wc -l)
    say "EQUIVALENCE_BYTES differing=$nd outside_provenance_field=$oof"
    [ "$oof" = 0 ] && say "EQUIVALENCE_SCOPE PASS" || bad "EQUIVALENCE_SCOPE: bytes differ outside the provenance field"
  else
    bad "the reference container has no readable m20 ($REF_CONTAINER): $(cat "$D/m20-posthoc.err")"
  fi
  "$X" trie-info "$A" | tee "$D/info-inbuild.txt"
  "$X" trie-info "$REF_CONTAINER" | tee "$D/info-posthoc.txt"
  diff "$D/info-inbuild.txt" "$D/info-posthoc.txt" > /dev/null && say "TRIE_INFO_IDENTICAL PASS" || bad "TRIE_INFO differs"
else
  bad "no reference container at $REF_CONTAINER"
fi

echo
if [ $fail -eq 0 ]; then echo "G2_RESULT PASS"; else echo "G2_RESULT FAIL"; exit 1; fi
