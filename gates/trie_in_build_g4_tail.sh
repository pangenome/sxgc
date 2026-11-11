#!/usr/bin/env bash
# T4 coverage + the wide-threshold arm + THE 10 GB POST-HOC REFERENCE.
#
#   1. branch-of --batch, 1,000 random 8-byte contexts, on the in-build 10 GB
#      container AND on the post-hoc container (same batch), side by side.
#   2. the wide arm (--trie-min-support 100) at 150 MB: build + ctx-build
#      reference + the same equivalence gate, so the exposed flag is exercised
#      and not merely parsed.
#   3. `xsa ctx-build` over the 10 GB container with the banked post-hoc suffix
#      array (--sa-cache): the SAME-CODE post-hoc member, so the 10 GB
#      equivalence does not rest on a container banked before member format v2.
#
# usage: gates/trie_in_build_g4_tail.sh
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
D=${D:-/mnt/nvme2n1/erikg/trie-in-build}
CORPUS10=${CORPUS10:-/mnt/nvme2n1/erikg/pf-10gb.txt}
CORPUS150=${CORPUS150:-/dev/shm/t150m.txt}
SA10=${SA10:-/mnt/nvme2n1/erikg/ctx-trie/pf10gb.sa5}
fail=0
say() { echo "G4_$*"; }
bad() { echo "G4_FAIL $*"; fail=1; }

echo "G4_BOX $(uptime)"
# ---- 1. coverage, in-build vs post-hoc, SAME batch
D=$D/g10 "$ROOT/gates/trie_in_build_g3_coverage.sh" "$D/g10/trie/pf-10gb.sxi2" 1000 || bad "coverage run"

# ---- 2. the wide arm at 150 MB
W=$D/g4/ms100
rm -rf "$W"; mkdir -p "$W"
"$X" build --tape --input "$CORPUS150" --scratch "$W/s" --threads 24 --memory-gb 24 \
   --trie-min-support 100 > "$W/build.log" 2>&1 || bad "ms100 build"
"$X" build --tape --input "$CORPUS150" --scratch "$W/ref" --threads 24 --memory-gb 24 \
   --no-trie > "$W/refbuild.log" 2>&1 || bad "ms100 no-trie build"
grep -E "CONTEXT_TRIE_(DEPTH|PUBLISHED|TAP)" "$W/build.log" | cut -c1-300
"$X" ctx-build --sxi "$W/ref/t150m.sxi2" --text "$CORPUS150" --out "$W/ref.sxi2" \
   --depth 8 --min-support 100 \
   > "$W/ref.log" 2>&1 || bad "ms100 ctx-build"
"$X" trie-member "$W/s/t150m.sxi2" -o "$W/a.bin"
"$X" trie-member "$W/ref.sxi2" -o "$W/b.bin"
a=$(sed 's/.*sha256_noprov=\([0-9a-f]*\).*/\1/' <("$X" trie-member "$W/s/t150m.sxi2"))
b=$(sed 's/.*sha256_noprov=\([0-9a-f]*\).*/\1/' <("$X" trie-member "$W/ref.sxi2"))
say "MS100_EQUIVALENCE in_build=$a post_hoc=$b"
[ "$a" = "$b" ] && say "MS100_EQUIVALENCE PASS" || bad "ms100 member differs"
nd=$(cmp -l "$W/a.bin" "$W/b.bin" | wc -l)
oof=$(cmp -l "$W/a.bin" "$W/b.bin" | awk '{o=$1} o<153 || o>168' | wc -l)
say "MS100_BYTES differing=$nd outside_provenance=$oof"
[ "$oof" = 0 ] && say "MS100_SCOPE PASS" || bad "ms100 differing bytes outside provenance"

# ---- 3. the post-hoc member of the SAME 10 GB container, same code
R=$D/g4/posthoc10
rm -rf "$R"; mkdir -p "$R"
echo "G4_POSTHOC10_START $(date -u +%H:%M:%S) (ctx-build with --sa-cache: the LF walk is skipped, the sweep runs)"
/usr/bin/time -f "G4_POSTHOC10_WALL %e s RSS %M kB" \
  "$X" ctx-build --sxi "$D/g10/notrie/pf-10gb.sxi2" --text "$CORPUS10" --out "$R/ref.sxi2" \
     --sa-cache "$SA10" --depth 8 --min-support 1000 --threads 32 > "$R/ref.log" 2>&1
rc=$?
grep -E "^G4_POSTHOC10_WALL|CONTEXT_TRIE depth|CONTEXT_TRIE_BUILD|SA_CACHE|CONTEXT_TRIE_SOURCE" "$R/ref.log"
[ $rc -eq 0 ] || bad "ctx-build 10 GB rc=$rc (see $R/ref.log)"
if [ $rc -eq 0 ]; then
  "$X" trie-member "$D/g10/trie/pf-10gb.sxi2" -o "$R/a.bin" | tee "$R/a.txt"
  "$X" trie-member "$R/ref.sxi2" -o "$R/b.bin" | tee "$R/b.txt"
  a=$(sed 's/.*sha256_noprov=\([0-9a-f]*\).*/\1/' "$R/a.txt")
  b=$(sed 's/.*sha256_noprov=\([0-9a-f]*\).*/\1/' "$R/b.txt")
  say "EQUIVALENCE_10G in_build=$a post_hoc=$b"
  [ "$a" = "$b" ] && say "EQUIVALENCE_10G_IDENTITY PASS" || bad "10 GB member payloads differ"
  python3 "$ROOT/gates/trie_member_cmp.py" same-code "$R/a.bin" "$R/b.bin" | sed 's/^/G4_/'
  [ "${PIPESTATUS[0]}" = 0 ] && say "EQUIVALENCE_10G_SCOPE PASS" || bad "10 GB differing bytes outside the provenance field"
fi
echo
if [ $fail -eq 0 ]; then echo "G4_RESULT PASS"; else echo "G4_RESULT FAIL"; exit 1; fi
