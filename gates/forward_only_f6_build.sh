#!/usr/bin/env bash
# F6 - THE BUILD STILL EMITS THE ONE SHAPE, AND IT READS BACK (forward-only gate).
#
# A fixture corpus + an independent XSEP v1 document map (gates/forward_only_fixture.c,
# no index code linked) go through `xsa build --tape`, and the gate then:
#   1. requires the driver's own certification line (CONTAINER_CERTIFIED, which is
#      the QUERY GATE reopening the published container),
#   2. requires v10 + m15 + m20 in the header,
#   3. answers query/search/consult/mem/branch-of/seeds/stats on it,
#   4. requires a build WITHOUT --docs-map to be refused,
#   5. requires a --no-trie build to answer query but REFUSE branch-of by name.
#
#   usage: gates/forward_only_f6_build.sh BINARY WORKDIR [BYTES] [DOCS]
set -u
X=${1:?binary}
D=${2:?workdir}
BYTES=${3:-64000000}
DOCS=${4:-4096}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
FIX=${FIX:-/tmp/forward_only_fixture}
[ -x "$FIX" ] || gcc -O2 -o "$FIX" "$HERE/forward_only_fixture.c" || exit 1
mkdir -p "$D"
rm -rf "$D"/build* "$D"/no-trie*
echo "F6_BOX $(uptime)"
echo "F6_BINARY $X $(sha256sum "$X" | cut -d' ' -f1)"

"$FIX" "$D/fix" "$BYTES" "$DOCS" | tee "$D/fixture.txt" || exit 1
CORPUS="$D/fix/out.bin"
MAP="$D/fix/map"

# ---- 1. the build, with its own certification line
echo "F6_RUN build"
setsid timeout --kill-after=10 3600 "$X" build --tape --input "$CORPUS" \
  --scratch "$D/build" --docs-map "$MAP" --memory-gb 8 --threads 4 \
  --trie-min-support 50 --trie-docs-min-support 50 > "$D/build.log" 2>&1
rc=$?
echo "F6_BUILD rc=$rc"
[ $rc -eq 0 ] || { tail -5 "$D/build.log"; echo "F6_FAIL build"; exit 1; }
grep -E "^CONTAINER_CERTIFIED" "$D/build.log" | tee "$D/certified.txt"
grep -q "CONTAINER_CERTIFIED format=sxi2-v10 version=10" "$D/build.log" \
  || { echo "F6_FAIL the driver did not certify SXI2 v10"; exit 1; }
grep -q "m15=" "$D/build.log" && grep -q "m20=" "$D/build.log" \
  || { echo "F6_FAIL the certification does not carry m15 and m20"; exit 1; }
C="$D/build/out.sxi2"
echo "F6_CONTAINER $(ls -l "$C")"
echo "F6_HEADER $(xxd -l 8 -p "$C")"
grep -E "^DOCSTARTS_JOIN|^CONTEXT_TRIE_JOIN" "$D/build.log"

n=0
ok=0
try() {
  name=$1; shift
  n=$((n+1))
  if setsid timeout --kill-after=10 600 "$@" > "$D/$name.out" 2> "$D/$name.err"; then
    ok=$((ok+1)); echo "F6_PASS name=$name bytes=$(stat -c%s "$D/$name.out")"
  else
    echo "F6_FAIL name=$name"; tail -3 "$D/$name.err"
  fi
}

# ---- 2. the read surface on the freshly built container
try query      "$X" query "$C" --count " the quick brown fox "
try mem        "$X" mem "$C" "quick brown" -k 2
try branch_of  "$X" branch-of "$C" "quick br"
try trie_info  "$X" trie-info "$C"
try docstarts  "$X" docstarts info "$C"
try stats      "$X" stats --sxi "$C"
printf '{"doc": 3, "start": 1024, "len": 16, "left": 1024}\n{"doc": 7, "start": 2048, "len": 16, "left": 1024}\n' > "$D/w.jsonl"
try consult    "$X" consult --container "$C" --windows "$D/w.jsonl" --corpus "$CORPUS" --output "$D/consult.rows"
tail -c 200 "$D/fixture.txt" > /dev/null
dd if="$CORPUS" of="$D/query.txt" bs=1 skip=4096 count=200 status=none
try seeds      "$X" seeds "$C" "$D/query.txt" --limit 4

# the query count must be the document count of the fixture's one repeated phrase
want=$(sed -n 's/.*docs=\([0-9]*\).*/\1/p' "$D/fixture.txt")
got=$(cut -f3 "$D/query.out" | tr -d '\r')
echo "F6_COUNT want=$want got=$got"
[ "$got" = "$want" ] || { echo "F6_FAIL the repeated phrase's count is not the doc count"; }
n=$((n+1)); [ "$got" = "$want" ] && ok=$((ok+1))

# ---- 3. a build without a document map is refused
echo "F6_RUN build-no-map"
setsid timeout --kill-after=10 300 "$X" build --tape --input "$CORPUS" --scratch "$D/build-nomap" \
  > "$D/nomap.out" 2> "$D/nomap.err"
rc=$?
n=$((n+1))
if [ $rc -ne 0 ] && grep -q -- "--docs-map is REQUIRED" "$D/nomap.err"; then
  ok=$((ok+1)); echo "F6_PASS name=build-without-docs-map rc=$rc"
else
  echo "F6_FAIL name=build-without-docs-map rc=$rc"; tail -3 "$D/nomap.err"
fi

# ---- 4. --no-trie: query answers, the trie commands REFUSE by name
echo "F6_RUN build-no-trie"
setsid timeout --kill-after=10 3600 "$X" build --tape --no-trie --input "$CORPUS" \
  --scratch "$D/no-trie" --docs-map "$MAP" --memory-gb 8 --threads 4 > "$D/notrie.log" 2>&1
rc=$?
n=$((n+1))
if [ $rc -eq 0 ]; then ok=$((ok+1)); echo "F6_PASS name=build-no-trie rc=0"; else
  echo "F6_FAIL name=build-no-trie rc=$rc"; tail -5 "$D/notrie.log"; fi
NT="$D/no-trie/out.sxi2"
n=$((n+1))
if "$X" query "$NT" --count "quick" > "$D/notrie_query.out" 2>"$D/notrie_query.err"; then
  ok=$((ok+1)); echo "F6_PASS name=no-trie-query answers"
else
  echo "F6_FAIL name=no-trie-query"; tail -3 "$D/notrie_query.err"
fi
n=$((n+1))
setsid timeout --kill-after=10 120 "$X" branch-of "$NT" "quick" > "$D/notrie_branch.out" 2> "$D/notrie_branch.err"
rc=$?
if [ $rc -ne 0 ] && grep -q "carries no member m20" "$D/notrie_branch.err" \
   && grep -q 'rebuild with `xsa build`' "$D/notrie_branch.err"; then
  ok=$((ok+1)); echo "F6_PASS name=no-trie-branch-of refuses by name rc=$rc"
  sed 's/^/    | /' "$D/notrie_branch.err"
else
  echo "F6_FAIL name=no-trie-branch-of rc=$rc"; tail -3 "$D/notrie_branch.err"
fi

echo "F6_CHECKS n=$n ok=$ok"
echo "F6_DONE"
[ "$n" = "$ok" ] || exit 1
