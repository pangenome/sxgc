#!/usr/bin/env bash
# Q6 gate: CLI safety of the v6-lite query path.
#
#  1. the container is POSITIONAL and there is no --ri4/--sxi flag any more
#  2. a legacy container is refused BY NAME with a migration line
#  3. the open line names the format (format=sxi2-v6-lite)
#  4. the legacy reader is NOT reachable for a lite container, proved three
#     ways: the source has no call into it, no legacy stage is traced at
#     runtime, and `query-legacy` refuses a v6 lite container by name.
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
XSA=${XSA:-$ROOT/xsa/target/release/xsa}
C=${C:-/mnt/nvme2n1/erikg/wip-lce150/t150m.sxi2}
PATS=$ROOT/tools/binbwt/pats.fa
LEGACY1=${LEGACY1:-/mnt/nvme2n1/erikg/sxgc-trend/slice8.sxi}
LEGACY2=${LEGACY2:-/tmp/sxi2-v4-tiny/compact.sxi}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
ck() { if [ "$1" = ok ]; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }

# 1. --ri4 / --sxi are gone
out=$(timeout 300 "$XSA" query --ri4 "$C" --patterns "$PATS" 2>&1); rc=$?
[ $rc -ne 0 ] && grep -q -- '--ri4/--sxi are gone' <<<"$out" && ck ok "--ri4 refused: $(head -1 <<<"$out")" \
    || { ck fail "--ri4 not refused (rc=$rc)"; }
out=$(timeout 300 "$XSA" query "$C" --sxi "$C" --patterns "$PATS" 2>&1); rc=$?
[ $rc -ne 0 ] && grep -q -- '--ri4/--sxi are gone' <<<"$out" && ck ok "--sxi refused" \
    || ck fail "--sxi not refused (rc=$rc)"

# 2. a legacy container is refused by name, with the rebuild line
if [ -r "$LEGACY1" ]; then
    out=$(timeout 300 "$XSA" query "$LEGACY1" --patterns "$PATS" 2>&1); rc=$?
    [ $rc -ne 0 ] && grep -q 'SXI1 v1 container is not a queryable index' <<<"$out" \
        && grep -q 'must be rebuilt with `xsa build`' <<<"$out" \
        && ck ok "SXI1 refused: $(head -1 <<<"$out")" || ck fail "SXI1 not refused (rc=$rc)"
else
    echo "SKIP: no SXI1 container at $LEGACY1"
fi
if [ -r "$LEGACY2" ]; then
    out=$(timeout 300 "$XSA" query "$LEGACY2" --patterns "$PATS" 2>&1); rc=$?
    [ $rc -ne 0 ] && grep -q 'phi-era container) is not a queryable index' <<<"$out" \
        && ck ok "SXI2 v2 refused: $(head -1 <<<"$out")" || ck fail "SXI2 v2 not refused (rc=$rc)"
else
    echo "SKIP: no SXI2 v2 container at $LEGACY2"
fi

# 3./4a. the open line names the format, and the lite stages are the only ones
#        traced (no legacy open stage ever appears, even with XSA_TRACE=1)
head -2 "$PATS" >"$TMP/one.fa"
XSA_TRACE=1 timeout 900 "$XSA" query "$C" --patterns-fasta "$TMP/one.fa" -j 4 \
    >"$TMP/one.tsv" 2>"$TMP/one.err"
grep -q 'format=sxi2-v6-lite' "$TMP/one.err" && ck ok "open line names the format" \
    || ck fail "open line does not name the format"
grep -q 'QLITE_OPEN .*decoded_bytes=0 stages=none' "$TMP/one.err" \
    && ck ok "open is mmap-only (decoded_bytes=0, stages=none)" \
    || ck fail "open is not mmap-only"
if grep -Eq 'sxi\.runs-decode|ri4\.runs-decode|sxi\.chi-decode|sxi\.lf-starts|ri4\.cruns' "$TMP/one.err"; then
    ck fail "a legacy open stage ran: $(grep -E 'sxi\.|ri4\.' "$TMP/one.err" | head -2 | tr '\n' ' ')"
else
    ck ok "no legacy open/load stage ran (no sxi.runs-decode / ri4.* in the trace)"
fi
grep -q 'lite.m1-runs\|stage=lite\.' "$TMP/one.err" && ck ok "lite stages traced" \
    || ck fail "no lite stage traced"

# 4b. the source has no path from the query command into the legacy reader
# (code lines only: the module docs *describe* the legacy reader by name)
if grep -vE '^\s*(//|$)' "$ROOT/xsa/src/qlite.rs" | grep -Eq 'product::|cmd_query|sxi::Container|Ri4'; then
    ck fail "qlite.rs references the legacy reader"
else
    ck ok "qlite.rs has no reference to product::/cmd_query/sxi::Container"
fi
grep -q 'Some("query") => qlite::command' "$ROOT/xsa/src/main.rs" \
    && ck ok "'query' dispatches to qlite::command only" || ck fail "query dispatch"

# 4c. query-legacy is not a back door for the published format
out=$(timeout 300 "$XSA" query-legacy --sxi "$C" --patterns "$PATS" 2>&1); rc=$?
[ $rc -ne 0 ] && grep -q 'is an SXI2 v6 lite container' <<<"$out" \
    && ck ok "query-legacy refuses a v6 lite container" || ck fail "query-legacy accepted a lite container (rc=$rc)"

if [ $fail -eq 0 ]; then echo "GATE Q6 PASS"; else echo "GATE Q6 FAIL"; exit 1; fi
