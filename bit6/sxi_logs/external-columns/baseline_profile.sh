#!/usr/bin/env bash
# Baseline (resident) fragment finish profile: per-phase wall, /usr/bin/time -v
# peak RSS, plus a 1 Hz VmRSS/VmHWM sampler correlated with SLIM_PHASE lines.
# Banked merged artifacts are read-only inputs; outputs under /tmp/extcols/frag-base.
set -uo pipefail
BANK=/home/erikg/cross-lcp-run/merged
OUT=/tmp/extcols/frag-base
BIN=/tmp/extcols/base
J=bit6/sxi_logs/external-columns
XSA=/home/erikg/sxgc/xsa/target/release/xsa
mkdir -p "$OUT"

sample_rss() { # sample_rss PID OUT.tag
    local pid=$1 tag=$2
    while kill -0 "$pid" 2>/dev/null; do
        local rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null)
        local hwm=$(awk '/VmHWM/{print $2}' /proc/$pid/status 2>/dev/null)
        echo "$(date +%s) $rss $hwm" >> "$OUT/$tag.rss"
        sleep 1
    done
}

run_phase() { # run_phase TAG out.log cmd...
    local tag=$1 log=$2; shift 2
    echo "=== $tag start $(date +%s)" >> "$OUT/phases.txt"
    /usr/bin/time -v -o "$OUT/$tag.time" "$@" > "$log" 2>&1 &
    local pid=$!
    sample_rss "$pid" "$tag" &
    local spid=$!
    wait "$pid"; local rc=$?
    kill "$spid" 2>/dev/null
    echo "=== $tag end $(date +%s) rc=$rc" >> "$OUT/phases.txt"
    return $rc
}

run_phase endpoints "$OUT/endpoints.log" \
    "$BIN/rpfbwt_endpoints" "$BANK/frag" "$OUT/fresh.ri4" "$OUT/fresh.head_sa" || exit 1
grep -q ENDPOINT_PASS "$OUT/endpoints.log" || { echo ENDPOINTS_FAIL; tail -5 "$OUT/endpoints.log"; exit 1; }
cmp "$OUT/fresh.ri4" "$BANK/fresh.ri4" && echo BASE_ENDPOINTS_BYTE_IDENTICAL
cmp "$OUT/fresh.head_sa" "$BANK/fresh.head_sa" && echo BASE_HEADSA_BYTE_IDENTICAL

run_phase slim "$OUT/slim.log" \
    "$BIN/slim_dump" --slim --resolve-ri4 --ri4 "$OUT/fresh.ri4" \
    --head-sa "$OUT/fresh.head_sa" -t 48 -o "$OUT/fresh.agg" || exit 1
cmp "$OUT/fresh.agg" "$BANK/fresh.agg" && echo BASE_AGG_BYTE_IDENTICAL

run_phase sweep "$OUT/sweep.log" \
    "$XSA" chi-rspace --stream-agg --ri4 "$OUT/fresh.ri4" --agg "$OUT/fresh.agg" -o "$OUT/fresh.sA" || exit 1
grep -F 'chi = 306164765 (N=1082130213, R=397723010)' "$OUT/sweep.log" && echo BASE_CHI_OK
cmp "$OUT/fresh.sA" "$BANK/fresh.sA" && echo BASE_sA_BYTE_IDENTICAL
echo BASELINE_PROFILE_DONE
