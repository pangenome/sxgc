#!/usr/bin/env bash
# Q5 gate: the v6-lite query reader on the 10 GB pile container, low-count rows
# only (>=10 patterns with oracle count <= 3, counts AND positions exact).
#
# Container: /mnt/nvme2n1/erikg/r10g-m96/pile-10b-snap.sxi2
#            (n=10000017259, R=3474889847, 4.5 GB, v6 lite m1,m4,m5,m7,m10,m11)
# Oracle:    /tmp/g10/oracle.tsv (idx<TAB>true_count<TAB>pos,pos,...)
#            /tmp/g10/pats.fa   (FASTA, same idx order)
#
# High-count patterns are excluded on purpose: one row of this oracle reports
# 16,365,874 occurrences, i.e. tens of MB of position output, which measures
# formatting and not the kernel.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
XSA=${XSA:-$ROOT/xsa/target/release/xsa}
C=${C:-/mnt/nvme2n1/erikg/r10g-m96/pile-10b-snap.sxi2}
ORACLE=${ORACLE:-/tmp/g10/oracle.tsv}
PATS=${PATS:-/tmp/g10/pats.fa}
J=${J:-32}
N=${N:-20}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -r "$ORACLE" ] && [ -r "$PATS" ] || { echo "SKIP: no 10 GB oracle at $ORACLE / $PATS"; exit 0; }

# the N lowest-count rows, ties broken by index, count <= 3
awk -F'\t' -v n="$N" 'NR>1 && $2<=3 {print $2"\t"$1}' "$ORACLE" | sort -k1,1n -k2,2n \
    | head -n "$N" | cut -f2 >"$TMP/ids"
nc=$(wc -l <"$TMP/ids")
[ "$nc" -ge 10 ] || { echo "FAIL: only $nc low-count oracle rows"; exit 1; }

# pats.fa headers are >q000..; the oracle's idx is the RECORD ORDINAL (0-based),
# which is what we key on.
awk -v ids="$TMP/ids" 'BEGIN{while((getline l < ids)>0) want[l+0]=1}
    /^>/{ord=hdr++; keep=(ord in want); if(keep){n++; printf ">p%d\n", n-1}; next}
    keep{print}' "$PATS" >"$TMP/pats.fa"
grep -c '^>' "$TMP/pats.fa" | grep -qx "$nc" || { echo "FAIL: pattern extraction"; exit 1; }
awk -F'\t' -v ids="$TMP/ids" 'BEGIN{while((getline l < ids)>0) want[l+0]=1}
    ($1+0) in want {print}' "$ORACLE" >"$TMP/oracle.tsv"

echo "== gate: qlite 10 GB correctness ($nc low-count patterns, -j $J)"
/usr/bin/time -f "WALL %e s  RSS %M kB" timeout 900 "$XSA" query "$C" \
    --patterns-fasta "$TMP/pats.fa" -j "$J" >"$TMP/pos.tsv" 2>"$TMP/err.txt"
rc=$?
grep -E 'QLITE_OPEN|QLITE_BUILD|QUERY_DONE' "$TMP/err.txt"
grep -E '^WALL' "$TMP/err.txt"
[ $rc -eq 0 ] || { echo "FAIL: query exited $rc"; tail -5 "$TMP/err.txt"; exit 1; }

# our pattern ids are 0..n-1 in oracle-row order; compare per-outer-index
fail=0
i=0
: >"$TMP/pos.mine"
: >"$TMP/pos.oracle"
while read -r idx; do
    awk -F'\t' -v p="$i" '$1==p {print $2}' "$TMP/pos.tsv" | sort -n >"$TMP/one.mine"
    awk -F'\t' -v idx="$idx" 'NR>1 && $1==idx {n=split($3,a,","); for(j=1;j<=n;j++) print a[j]}' \
        "$ORACLE" | sort -n >"$TMP/one.oracle"
    # counts
    printf '%d\t%s\n' "$i" "$(wc -l <"$TMP/one.mine")" >>"$TMP/pos.mine"
    printf '%d\t%s\n' "$i" "$(wc -l <"$TMP/one.oracle")" >>"$TMP/pos.oracle"
    if ! diff -q "$TMP/one.mine" "$TMP/one.oracle" >/dev/null; then
        echo "POSITION MISMATCH pattern $i (oracle idx $idx)"
        diff "$TMP/one.mine" "$TMP/one.oracle" | head -5
        fail=1
    fi
    i=$((i + 1))
done <"$TMP/ids"

if diff -q "$TMP/pos.mine" "$TMP/pos.oracle" >/dev/null && [ $fail -eq 0 ]; then
    echo "COUNTS $nc/$nc exact"
    echo "POSITIONS $nc/$nc exact"
else
    echo "COUNTS MISMATCH"; fail=1
fi
grep -q 'QLITE_OPEN .*decoded_bytes=0 stages=none' "$TMP/err.txt" \
    || { echo "FAIL: open is not mmap-only"; fail=1; }

if [ $fail -eq 0 ]; then echo "GATE Q5 PASS"; else echo "GATE Q5 FAIL"; exit 1; fi
