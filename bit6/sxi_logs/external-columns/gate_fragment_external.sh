#!/usr/bin/env bash
# FRAGMENT gate for the EXTERNAL finish: the full chain (endpoints -> slim ->
# sweep) with all run-scale columns external (bit6/ext_columns.hpp).
#   (a) byte identity: ri4/head_sa/agg/sA byte-identical to the banked
#       artifacts, chi = 306164765 exactly;
#   (b) memory force-test: the same chain under a hard RLIMIT_AS cap of
#       $CAP_KB KiB (default 8 GiB — the resident version holds ~33 GB
#       (endpoints) / ~19.4 GB (slim) and cannot even start under the cap).
# Banked merged artifacts are read-only inputs; outputs under /tmp/extcols/frag.
set -uo pipefail
BANK=/home/erikg/cross-lcp-run/merged
BIN=/tmp/extcols/bin
XSA=/home/erikg/sxgc/xsa/target/release/xsa
J=bit6/sxi_logs/external-columns
CAP_KB=${CAP_KB:-8388608}
OUT=/tmp/extcols/frag
mkdir -p "$OUT"

sample_rss() { # $1 pid of /usr/bin/time, $2 tag — samples the timed CHILD
    local ppid=$1 tag=$2
    while kill -0 "$ppid" 2>/dev/null; do
        for pid in $(pgrep -P "$ppid" 2>/dev/null); do
            local rss=$(awk '/VmRSS/{print $2}' /proc/$pid/status 2>/dev/null)
            [ -n "$rss" ] && echo "$(date +%s) $rss" >> "$OUT/$tag.rss"
        done
        sleep 2
    done
}

run_phase() { # tag logfile cmd... (the cmd runs under the current ulimit)
    local tag=$1 log=$2; shift 2
    echo "=== $tag start $(date +%s) cap_kb=$(ulimit -v)" >> "$OUT/phases.txt"
    /usr/bin/time -v -o "$OUT/$tag.time" "$@" > "$log" 2>&1 &
    local pid=$!
    sample_rss "$pid" "$tag" &
    local spid=$!
    wait "$pid"; local rc=$?
    kill "$spid" 2>/dev/null
    echo "=== $tag end $(date +%s) rc=$rc" >> "$OUT/phases.txt"
    return $rc
}

finish_chain() { # $1 = tag prefix dir already set by caller
    run_phase endpoints "$OUT/endpoints.log" \
        "$BIN/rpfbwt_endpoints" "$BANK/frag" "$OUT/fresh.ri4" "$OUT/fresh.head_sa" || return 1
    grep -q ENDPOINT_PASS "$OUT/endpoints.log" || { echo ENDPOINTS_FAIL; return 1; }
    cmp "$OUT/fresh.ri4" "$BANK/fresh.ri4" && cmp "$OUT/fresh.head_sa" "$BANK/fresh.head_sa" \
        && echo "$1 ENDPOINTS_BYTE_IDENTICAL" || { echo "$1 ENDPOINTS_MISMATCH"; return 1; }

    run_phase slim "$OUT/slim.log" \
        "$BIN/slim_dump" --slim --resolve-ri4 --ri4 "$OUT/fresh.ri4" \
        --head-sa "$OUT/fresh.head_sa" -t 48 -o "$OUT/fresh.agg" || return 1
    cmp "$OUT/fresh.agg" "$BANK/fresh.agg" && echo "$1 AGG_BYTE_IDENTICAL" || { echo "$1 AGG_MISMATCH"; return 1; }

    run_phase sweep "$OUT/sweep.log" \
        "$XSA" chi-rspace --stream-agg --ri4 "$OUT/fresh.ri4" --agg "$OUT/fresh.agg" -o "$OUT/fresh.sA" || return 1
    grep -F 'chi = 306164765 (N=1082130213, R=397723010)' "$OUT/sweep.log" \
        && echo "$1 CHI_306164765_OK" || { echo "$1 CHI_MISMATCH"; return 1; }
    cmp "$OUT/fresh.sA" "$BANK/fresh.sA" && echo "$1 sA_BYTE_IDENTICAL" || { echo "$1 sA_MISMATCH"; return 1; }
}

echo "########## (a) external finish, uncapped"
rm -f "$OUT"/fresh.ri4 "$OUT"/fresh.head_sa "$OUT"/fresh.agg "$OUT"/fresh.sA
finish_chain GATE_A || { echo GATE_A_FAIL; exit 1; }

echo "########## (b) memory force-test under RLIMIT_AS ${CAP_KB} KiB"
OUT=/tmp/extcols/frag-cap
mkdir -p "$OUT"
rm -f "$OUT"/fresh.ri4 "$OUT"/fresh.head_sa "$OUT"/fresh.agg "$OUT"/fresh.sA
( ulimit -v "$CAP_KB"; finish_chain GATE_B ) || { echo GATE_B_FAIL; exit 1; }

echo "########## resident-version refusal under the same cap (evidence)"
OUT=/tmp/extcols/frag-refuse
mkdir -p "$OUT"
( ulimit -v "$CAP_KB"; ulimit -v; /tmp/extcols/base/rpfbwt_endpoints "$BANK/frag" \
    "$OUT/fresh.ri4" "$OUT/fresh.head_sa" > "$OUT/endpoints.log" 2>&1 )
echo "resident endpoints under cap rc=$? (expected nonzero)"
tail -2 "$OUT/endpoints.log"
echo GATE_AB_DONE
