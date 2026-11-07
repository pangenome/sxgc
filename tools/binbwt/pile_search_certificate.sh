#!/usr/bin/env bash
# Pile search certificate: does the index answer from the corpus, and who is
# right when `grep -o` disagrees?
#
# The 100 GB Pile container answered 4,785,152 occurrences of " function " while
# `LC_ALL=C grep -a -o -F` answered 4,784,960. Three other patterns agreed
# exactly. This script settles it with three INDEPENDENT enumerations of the same
# occurrences and names every offset that differs:
#
#   1. `xsa verify`      - the index locates positions, then each one is read back
#                          out of the corpus and compared with the pattern.
#   2. `pile_pos_scan`   - a windowed-mmap scanner that shares no code with either
#                          the index or grep; it also histograms the byte before
#                          and after every match, which is what decomposes the
#                          count by "which character precedes the match".
#   3. `LC_ALL=C grep -a -b -o -F`
#
# Usage:
#   tools/binbwt/pile_search_certificate.sh CONTAINER CORPUS PATTERN [SAMPLE]
#
# SAMPLE is how many index positions to locate and byte-verify (default 20000;
# "all" locates every occurrence, which is hours at 100 GB scale - a locate
# walks LF one row at a time over a 31 G-run stream).
#
# Output and all intermediate products go to $WORK (default a fresh /tmp dir,
# kept and printed). Exit: 0 corpus agrees with the index, 1 it does not, 2 usage.
set -u
if [ $# -lt 3 ]; then
    sed -n '2,28p' "${BASH_SOURCE[0]}" >&2
    exit 2
fi
CONTAINER=$1
CORPUS=$2
PATTERN=$3
SAMPLE=${4:-20000}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
XSA=${XSA:-$ROOT/xsa/target/release/xsa}
WORK=${WORK:-/tmp/pile-search-certificate-$$}
JOBS=${JOBS:-64}
mkdir -p "$WORK" || exit 1
SCAN=$WORK/pile_pos_scan
POSSET=$WORK/posset
cc -O2 -o "$SCAN" "$ROOT/tools/binbwt/pile_pos_scan.c" || exit 1
cc -O2 -o "$POSSET" "$ROOT/tools/binbwt/posset.c" || exit 1
echo "WORK $WORK"

# ---------------------------------------------------------------- 1. the index
LIMIT_ARGS=()
if [ "$SAMPLE" != "all" ]; then LIMIT_ARGS=(--limit "$SAMPLE"); fi
/usr/bin/time -f "XSA_WALL %e s XSA_RSS %M kB" \
    "$XSA" verify "$CONTAINER" --corpus "$CORPUS" "${LIMIT_ARGS[@]}" -j "$JOBS" "$PATTERN" \
    >"$WORK/xsa.tsv" 2>"$WORK/xsa.err"
xsarc=$?
grep -E 'QLITE_OPEN|QLITE_BUILD|VERIFY_TOTAL|VERIFY_BAD|XSA_WALL' "$WORK/xsa.err"
[ $xsarc -eq 0 ] || { echo "FAIL: xsa verify exited $xsarc (see $WORK/xsa.err)"; exit 1; }
awk -F'\t' '{print $2}' "$WORK/xsa.tsv" | sort -n >"$WORK/xsa.txt"
echo "XSA_POSITIONS $(wc -l <"$WORK/xsa.txt") unique $(sort -u "$WORK/xsa.txt" | wc -l)"

# ------------------------------------------------------- 2. the independent scan
"$SCAN" "$CORPUS" "$PATTERN" --offsets "$WORK/scan.u64" >"$WORK/scan.txt" 2>"$WORK/scan.err"
grep -E '^SCAN' "$WORK/scan.txt"
"$POSSET" verify "$CORPUS" "$PATTERN" "$WORK/scan.u64" | tee "$WORK/scan_verify.txt"
"$POSSET" sort "$WORK/scan.u64" "$WORK/scan.sorted.u64" 2>/dev/null

# ------------------------------------------------------------------- 3. GNU grep
LC_ALL=C grep -a -b -o -F -- "$PATTERN" "$CORPUS" >"$WORK/grep.txt" 2>"$WORK/grep.err"
"$POSSET" fromtext "$WORK/grep.txt" "$WORK/grep.u64" 2>/dev/null
"$POSSET" sort "$WORK/grep.u64" "$WORK/grep.sorted.u64" 2>/dev/null
echo "GREP_OCCURRENCES $(wc -l <"$WORK/grep.txt")"

# ------------------------------------------------------------------- the ledger
"$POSSET" stat "$WORK/scan.sorted.u64"
"$POSSET" stat "$WORK/grep.sorted.u64"
"$POSSET" diff "$WORK/scan.sorted.u64" "$WORK/grep.sorted.u64" "$WORK/scan_not_grep.u64"
"$POSSET" diff "$WORK/grep.sorted.u64" "$WORK/scan.sorted.u64" "$WORK/grep_not_scan.u64"

# every offset the index answered must exist in the independent scan
python3 -c 'import struct,sys
vals=[int(x) for x in open(sys.argv[1])]
open(sys.argv[2],"wb").write(b"".join(struct.pack("<Q",v) for v in vals))' "$WORK/xsa.txt" "$WORK/xsa.u64"
"$POSSET" sort "$WORK/xsa.u64" "$WORK/xsa.sorted.u64" 2>/dev/null
xscan=$("$POSSET" diff "$WORK/xsa.sorted.u64" "$WORK/scan.sorted.u64" "$WORK/xsa_not_scan.u64")
echo "XSA_NOT_IN_SCAN $xscan"

# the offsets grep missed, with context
grep_missed=$("$POSSET" stat "$WORK/scan_not_grep.u64" | awk '{print $3}' | sed 's/unique=//')
echo "GREP_MISSED $grep_missed"
if [ "${grep_missed:-0}" != "0" ]; then
    "$POSSET" context "$CORPUS" "$PATTERN" "$WORK/scan_not_grep.u64" 40 | head -20
fi

ok=1
grep -q "bad=0" "$WORK/scan_verify.txt" || { echo "FAIL: the scan's own offsets do not carry the pattern"; ok=0; }
grep -qE '^VERIFY_TOTAL .*bad=0 duplicates=0' "$WORK/xsa.err" || { echo "FAIL: index positions failed the byte certificate"; ok=0; }
case "$xscan" in
    *a_not_b=0) ;;
    *) echo "FAIL: the index answered a position the independent scan does not have"; ok=0 ;;
esac

if [ $ok -eq 1 ]; then
    echo "CERTIFICATE PASS (work dir kept: $WORK)"
    exit 0
fi
echo "CERTIFICATE FAIL (work dir kept: $WORK)"
exit 1
