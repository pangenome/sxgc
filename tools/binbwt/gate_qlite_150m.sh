#!/usr/bin/env bash
# Q1 gate: the v6-lite query reader against the 150 MB fixture oracle.
#
# Container: /mnt/nvme2n1/erikg/wip-lce150/t150m.sxi2  (n=150000002, R=57279194)
# Oracle:    tools/binbwt/oracle.tsv  (100 patterns, idx<TAB>count<TAB>pos,pos,...)
#            independently re-verified against the corpus (/dev/shm/t150m.txt):
#            574/574 positions land on the pattern and all 100 counts agree.
#
# PASS requires: exact count for all 100 patterns AND exact position multiset
# for every pattern (574 positions), plus the auditable open line showing a
# mmap-only startup.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
XSA=${XSA:-$ROOT/xsa/target/release/xsa}
C=${C:-/mnt/nvme2n1/erikg/wip-lce150/t150m.sxi2}
PATS=$ROOT/tools/binbwt/pats.fa
ORACLE=$ROOT/tools/binbwt/oracle.tsv
J=${J:-16}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "== gate: qlite 150 MB correctness (C=$C, -j $J)"
timeout 900 "$XSA" query "$C" --patterns-fasta "$PATS" -j "$J" \
    >"$TMP/pos.tsv" 2>"$TMP/err.txt"
rc=$?
grep -E 'QLITE_OPEN|QLITE_BUILD|QUERY_DONE' "$TMP/err.txt"
[ $rc -eq 0 ] || { echo "FAIL: query exited $rc"; exit 1; }

# counts
awk -F'\t' '{cnt[$1]++} END {for (k in cnt) print k"\t"cnt[k]}' "$TMP/pos.tsv" \
    | sort -k1,1n >"$TMP/counts.mine"
awk -F'\t' 'NR>1{print $1"\t"$2}' "$ORACLE" | sort -k1,1n >"$TMP/counts.oracle"
# positions (multiset, sorted per pattern)
sort -k1,1n -k2,2n "$TMP/pos.tsv" >"$TMP/pos.mine"
awk -F'\t' 'NR>1{n=split($3,a,","); for(i=1;i<=n;i++) printf "%d\t%s\n",$1,a[i]}' "$ORACLE" \
    | sort -k1,1n -k2,2n >"$TMP/pos.oracle"

fail=0
if diff -q "$TMP/counts.mine" "$TMP/counts.oracle" >/dev/null; then
    echo "COUNTS 100/100 exact"
else
    echo "COUNTS MISMATCH:"; diff "$TMP/counts.mine" "$TMP/counts.oracle" | head -20; fail=1
fi
n=$(wc -l <"$TMP/pos.mine")
if diff -q "$TMP/pos.mine" "$TMP/pos.oracle" >/dev/null; then
    echo "POSITIONS $n/$n exact"
else
    echo "POSITION MISMATCH:"; diff "$TMP/pos.mine" "$TMP/pos.oracle" | head -20; fail=1
fi

grep -q 'QLITE_OPEN .*decoded_bytes=0 stages=none' "$TMP/err.txt" \
    || { echo "FAIL: open line does not show an mmap-only startup"; fail=1; }
grep -q 'format=sxi2-v6-lite' "$TMP/err.txt" \
    || { echo "FAIL: open line does not name the format"; fail=1; }

if [ $fail -eq 0 ]; then echo "GATE Q1 PASS"; else echo "GATE Q1 FAIL"; exit 1; fi
