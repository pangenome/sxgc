#!/usr/bin/env bash
# TIME LEVER (i) gate: merge-time fingerprint streaming.
# Re-runs the 16-chunk fragment tree merge with cross_lcp_merge --emit-pf:
# the FINAL pass emits the normalized text (.pftext) and tau-spaced suffix-hash
# checkpoints (.pfck) as side streams. Gates:
#   1. the four merged files stay byte-identical to the banked artifacts;
#   2. slim_dump --pf-text/--pf-checkpoints (ADOPT mode, no walk, no seeds)
#      reproduces the banked .agg byte-identically;
#   3. the .pfck is validated by an independent python recomputation over the
#      emitted text (sampled entries);
#   4. wall comparison: adopt-mode slim vs the walk-mode slim (gate A).
# Banked chunks + merged artifacts are read-only inputs.
set -uo pipefail
CHUNKS=/home/erikg/sxgc/vendor/chunk-merge-v3/chunks
BANK=/home/erikg/cross-lcp-run/merged
BIN=/tmp/extcols/bin
OUT=/tmp/extcols/mergerun
N=1082130213
mkdir -p "$OUT/frag" "$OUT/work"
rm -f "$OUT/frag"/frag.* "$OUT"/frag.pftext "$OUT"/frag.pfck

echo "=== merge --tree --emit-pp started $(date +%s)"
/usr/bin/time -v -o "$OUT/merge.time" \
    "$BIN/cross_lcp_merge" --tree "$CHUNKS" 16 "$N" "$OUT/frag/frag" \
    --threads 24 --work "$OUT/work" --emit-pf > "$OUT/merge.log" 2>&1
rc=$?
echo "=== merge done $(date +%s) rc=$rc"
[ $rc -eq 0 ] || { echo MERGE_FAIL; tail -5 "$OUT/merge.log"; exit 1; }
for f in rlebwt rlebwt.meta ssa ssa_t; do
    cmp "$OUT/frag/frag.$f" "$BANK/frag.$f" && echo "MERGE_FOUR_$f BYTE_IDENTICAL" || { echo "MERGE_$f MISMATCH"; exit 1; }
done
grep CROSS_PF_EMIT "$OUT/merge.log" | tail -1

# Independent checkpoint verification (sampled): the window identity the
# slim's own startup self-check uses — h(k) - BASE^w * h(k+w) must equal the
# direct hash of the w text bytes at k*tau. O(tau) per sampled entry, so the
# check is polylog, never a suffix rehash.
python3 - "$OUT/frag" <<'PY'
import struct, sys, random
d = sys.argv[1]
text = open(d + '/frag.pftext', 'rb').read()
ck = open(d + '/frag.pfck', 'rb').read()
magic, ver, n, tau, count, rsv = struct.unpack_from('<IIQQQQ', ck, 0)
assert magic == 0x4B434650 and ver == 1 and n == len(text) and rsv == 0, (magic, ver, n, rsv)
assert count == n // tau + 1 and len(ck) == 40 + 8 * count
BASE = 0x9e3779b185ebca87
def H(k):
    return struct.unpack_from('<Q', ck, 40 + 8 * k)[0]
random.seed(20261003)
checked = 0
for _ in range(2000):
    k = random.randrange(count - 1)
    w = tau if (k + 1) * tau <= n else n - k * tau
    seg = text[k * tau : k * tau + w]
    direct = 0
    for b in reversed(seg):            # backward fold: FIRST byte lands at BASE^0
        direct = (b + 1 + BASE * direct) % (1 << 64)
    pw = pow(BASE, w, 1 << 64)
    lhs = (H(k) - pw * H(k + 1)) % (1 << 64)
    assert lhs == direct, (k, w, lhs, direct)
    checked += 1
print(f'PFCK_PY_CHECK PASS tau={tau} count={count} n={n} entries={checked} window-identity verified')
PY
[ $? -eq 0 ] || { echo PFCK_CHECK_FAIL; exit 1; }

# ADOPT-mode slim: consume the merge sidecars, no walk.
/usr/bin/time -v -o "$OUT/slim-adopt.time" \
    "$BIN/slim_dump" --slim --resolve-ri4 --ri4 "$BANK/fresh.ri4" \
    --head-sa "$BANK/fresh.head_sa" -t 48 \
    --pf-text "$OUT/frag/frag.pftext" --pf-checkpoints "$OUT/frag/frag.pfck" \
    -o "$OUT/adopt.agg" > "$OUT/slim-adopt.log" 2>&1 || { echo ADOPT_SLIM_FAIL; tail -5 "$OUT/slim-adopt.log"; exit 1; }
cmp "$OUT/adopt.agg" "$BANK/fresh.agg" && echo "ADOPT_AGG_BYTE_IDENTICAL" || { echo ADOPT_AGG_MISMATCH; exit 1; }
grep -E "mode=adopt|SLIM_FP parse-free-text" "$OUT/slim-adopt.log" | head -2
grep -E "SLIM_QUERY_PHASE|Elapsed" "$OUT/slim-adopt.time" | head -2
echo MERGE_LEVER_GATE_DONE
