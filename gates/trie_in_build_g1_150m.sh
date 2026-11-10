#!/usr/bin/env bash
# T1/T2 at 150 MB: the in-build trie (m20) must be the post-hoc trie, byte for byte.
#
#   1. `--no-trie` is the product-unchanged arm: SXI2 v7, 8 members, sha256 equal to
#      the banked default and chi 44,034,230 (the banked constant).
#   2. the default arm (trie ON) publishes a container whose m20 member exists and
#      whose journal line is CONTEXT_TRIE_DEPTH ... source=in-build.
#   3. THE EQUIVALENCE GATE: `xsa ctx-build` builds m20 from the SAME container and
#      corpus through the post-hoc path (LF reconstruction + sweep). The two members
#      must agree on EVERY byte except the 16-byte provenance field (head offsets
#      152..168: the member SAYS which ordered stream produced it), and the gate
#      proves that the differing bytes are exactly those.
#
# usage: gates/trie_in_build_g1_150m.sh
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
D=${D:-/mnt/nvme2n1/erikg/trie-in-build/g1}
CORPUS=${CORPUS:-/dev/shm/t150m.txt}
V6=${V6:-/mnt/nvme2n1/erikg/L150/t150m.sxi2}
BANKED_SHA=${BANKED_SHA:-60b56ffa3502d466b80f0dc8}
CHI=${CHI:-44034230}
THREADS=${THREADS:-24}
MEMGB=${MEMGB:-24}
rm -rf "$D"; mkdir -p "$D"
fail=0
say() { echo "T1_$*"; }
bad() { echo "T1_FAIL $*"; fail=1; }

header() { # $1 container -> "version members flags length"
  python3 - "$1" <<'PY'
import struct,sys
h=open(sys.argv[1],'rb').read(64)
print(struct.unpack_from('<I',h,4)[0], struct.unpack_from('<I',h,32)[0],
      struct.unpack_from('<Q',h,48)[0], struct.unpack_from('<Q',h,40)[0])
PY
}

echo "T0 box: $(uptime)"
sha256sum "$CORPUS"
"$X" ctx-tap-selftest > "$D/tap-selftest.log" 2>&1 && say "TAP_SELFTEST PASS" || bad "tap selftest"
tail -1 "$D/tap-selftest.log"

# ---------------------------------------------------------------- 1. --no-trie
echo "== build --no-trie (the product-unchanged arm)"
/usr/bin/time -f "G1_WALL_NOTRIE %e s RSS %M kB" \
  "$X" build --tape --input "$CORPUS" --scratch "$D/noTrie" --threads "$THREADS" \
     --memory-gb "$MEMGB" --chi-expect "$CHI" --no-trie > "$D/noTrie.log" 2>&1
rc=$?
grep -E "^G1_WALL_NOTRIE" "$D/noTrie.log"
grep -E "XSA_BUILD_DONE" "$D/noTrie.log"
[ $rc -eq 0 ] || bad "--no-trie build rc=$rc (see $D/noTrie.log)"
NOTRIE=$D/noTrie/t150m.sxi2
sha=$(sha256sum "$NOTRIE" | cut -d' ' -f1)
hdr=$(header "$NOTRIE")
say "NO_TRIE container=$NOTRIE sha256=$sha header=\"$hdr\""
case "$sha" in
  "$BANKED_SHA"*) say "NO_TRIE_IDENTICAL PASS sha256=$sha == banked $BANKED_SHA..." ;;
  *) bad "NO_TRIE_IDENTICAL: $sha != banked $BANKED_SHA..." ;;
esac
set -- $hdr
[ "$1" = 7 ] || bad "NO_TRIE_VERSION: version=$1, want 7"
[ "$2" = 8 ] || bad "NO_TRIE_MEMBERS: members=$2, want 8"

# ------------------------------------------------------------ 2. the trie arm
echo "== build with the trie (defaults: --trie-depth 8 --trie-min-support 1000)"
/usr/bin/time -f "G1_WALL_TRIE %e s RSS %M kB" \
  "$X" build --tape --input "$CORPUS" --scratch "$D/trie" --threads "$THREADS" \
     --memory-gb "$MEMGB" --chi-expect "$CHI" > "$D/trie.log" 2>&1
rc=$?
grep -E "^G1_WALL_TRIE" "$D/trie.log"
grep -E "CONTEXT_TRIE_(POLICY|DEPTH|COLUMNS|TAP|BUDGET|JOIN|PUBLISHED|BUILD_WALL)" "$D/trie.log"
grep -E "XSA_BUILD_DONE" "$D/trie.log"
[ $rc -eq 0 ] || bad "trie build rc=$rc (see $D/trie.log)"
TRIE=$D/trie/t150m.sxi2
hdr=$(header "$TRIE")
say "TRIE container=$TRIE header=\"$hdr\""
if [ -z "$hdr" ]; then bad "TRIE_BUILD produced no container"; hdr="0 0 0 0"; fi
set -- $hdr
[ "$1" = 9 ] || bad "TRIE_VERSION: version=$1, want 9 (m20 present)"
[ "$2" = 9 ] || bad "TRIE_MEMBERS: members=$2, want 9"
grep -q "CONTEXT_TRIE_DEPTH .*source=in-build" "$D/trie.log" \
  && say "TRIE_JOURNAL PASS" || bad "TRIE_JOURNAL: no CONTEXT_TRIE_DEPTH ... source=in-build line"

# ------------------------------------------- 3. the post-hoc reference member
echo "== post-hoc ctx-build on the SAME container (LF cycle + sweep)"
/usr/bin/time -f "G1_WALL_REF %e s RSS %M kB" \
  "$X" ctx-build --sxi "$NOTRIE" --text "$CORPUS" --out "$D/ref.sxi2" --depth 8 --min-support 1000 \
  > "$D/ref.log" 2>&1
rc=$?
grep -E "^G1_WALL_REF|CONTEXT_TRIE_BUILD|CONTEXT_TRIE " "$D/ref.log"
[ $rc -eq 0 ] || bad "ctx-build rc=$rc (see $D/ref.log)"

# --------------------------------------------------------- 4. THE EQUIVALENCE
"$X" trie-member "$TRIE"   -o "$D/m20-inbuild.bin" | tee "$D/m20-inbuild.txt"
"$X" trie-member "$D/ref.sxi2" -o "$D/m20-posthoc.bin" | tee "$D/m20-posthoc.txt"
a=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^sha256_noprov=/){sub("sha256_noprov=","",$i); print $i}}' "$D/m20-inbuild.txt")
b=$(awk '{for(i=1;i<=NF;i++) if($i ~ /^sha256_noprov=/){sub("sha256_noprov=","",$i); print $i}}' "$D/m20-posthoc.txt")
say "EQUIVALENCE in_build_sha256_noprov=$a post_hoc_sha256_noprov=$b"
if [ "$a" = "$b" ] && [ -n "$a" ]; then say "EQUIVALENCE_IDENTITY PASS"; else bad "EQUIVALENCE: the member payloads differ"; fi
# and prove the ONLY differing bytes are the provenance field (head 152..168)
n_diff=$(cmp -l "$D/m20-inbuild.bin" "$D/m20-posthoc.bin" 2>/dev/null | wc -l)
out_of_field=$(cmp -l "$D/m20-inbuild.bin" "$D/m20-posthoc.bin" 2>/dev/null \
  | awk '{o=$1} o<153 || o>168 {print}' | wc -l)
say "EQUIVALENCE_BYTES differing_bytes=$n_diff outside_provenance_field=$out_of_field (cmp offsets are 1-based; field = 153..168)"
[ "$out_of_field" = 0 ] && say "EQUIVALENCE_SCOPE PASS" || bad "EQUIVALENCE_SCOPE: differing bytes outside the provenance field"
[ "$n_diff" -le 16 ] && say "EQUIVALENCE_PROV_ONLY PASS" || bad "EQUIVALENCE_PROV_ONLY: $n_diff bytes differ, provenance field is 16 bytes"

# ------------------------------------------------------------- 5. trie-info eq
"$X" trie-info "$TRIE"      | tee "$D/info-inbuild.txt"
"$X" trie-info "$D/ref.sxi2" | tee "$D/info-posthoc.txt"
if diff <(sed 's/ *$//' "$D/info-inbuild.txt") <(sed 's/ *$//' "$D/info-posthoc.txt") > /dev/null; then
  say "TRIE_INFO_IDENTICAL PASS"
else
  bad "TRIE_INFO: the two members report different counts"
fi

echo
if [ $fail -eq 0 ]; then echo "G1_RESULT PASS"; else echo "G1_RESULT FAIL"; exit 1; fi
