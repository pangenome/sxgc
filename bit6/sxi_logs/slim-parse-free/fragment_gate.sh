#!/usr/bin/env bash
# Gate (b)+(c): FRAGMENT — chi == 306,164,765 from the merged chunk-merge
# outputs with NO reference parse anywhere in the finish, plus memory/time
# profile. Banked artifacts are read-only inputs/oracles.
set -euo pipefail
J=bit6/sxi_logs/slim-parse-free
BANK=/home/erikg/cross-lcp-run/merged
OUT=/tmp/slimpf/frag
XSA=/home/erikg/sxgc/xsa/target/release/xsa
mkdir -p "$OUT"

# The finish consumes ONLY the merged four files. No .dict/.parse is opened.
test -s "$BANK/frag.rlebwt" && test -s "$BANK/frag.rlebwt.meta" \
    && test -s "$BANK/frag.ssa" && test -s "$BANK/frag.ssa_t"

/usr/bin/time -v -o "$OUT/endpoints.time" \
    /tmp/slimpf/rpfbwt_endpoints "$BANK/frag" "$OUT/fresh.ri4" "$OUT/fresh.head_sa" \
    > "$OUT/endpoints.log" 2>&1
grep -q ENDPOINT_PASS "$OUT/endpoints.log"
cmp "$OUT/fresh.ri4" "$BANK/fresh.ri4"
cmp "$OUT/fresh.head_sa" "$BANK/fresh.head_sa"
printf 'ENDPOINTS_PASS parse_free=1 byte_identical_to_banked\n'

/usr/bin/time -v -o "$OUT/slim.time" \
    /tmp/slimpf/slim_dump --slim --resolve-ri4 \
    --ri4 "$OUT/fresh.ri4" --head-sa "$OUT/fresh.head_sa" -t 48 \
    -o "$OUT/fresh.agg" > "$OUT/slim.log" 2>&1
cmp "$OUT/fresh.agg" "$BANK/fresh.agg"
printf 'SLIM_PASS parse_free=1 agg_byte_identical_to_banked\n'

/usr/bin/time -v -o "$OUT/sweep.time" \
    "$XSA" chi-rspace --stream-agg \
    --ri4 "$OUT/fresh.ri4" --agg "$OUT/fresh.agg" -o "$OUT/fresh.sA" \
    > "$OUT/sweep.log" 2>&1
grep -F 'chi = 306164765 (N=1082130213, R=397723010)' "$OUT/sweep.log"
test "$(stat -c %s "$OUT/fresh.sA")" = 2449318120
cmp "$OUT/fresh.sA" "$BANK/fresh.sA"
printf 'CHI_PASS count=306164765 bytes=2449318120 sA_byte_identical_to_banked\n'
printf 'FRAGMENT_GATE_PASS no_parse_anywhere=1\n'
