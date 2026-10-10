#!/usr/bin/env bash
# PILE-BUILDER v2 M1: the corpus-adapter identity gate.
# THE LAW (v2, supervisor-approved A/A): "never materialize text to disk".
# pftext WAS text on disk (the merge-time checkpoint emission was the one
# sanctioned exception); v2 eliminates the exception - the text lives exactly
# once, in the corpus, and the finish reads it through the read-only adapter
# T[i] = REMAP[corpus[i]] (no rotation; the merge's --emit-pf text M is the
# remapped corpus byte stream - verified here against the banked 1GB artifact).
# This script re-runs the one-time full byte-compare of the adapter stream
# against the banked mergperf-1b-B pftext. Caps: timeout.
set -uo pipefail
BASE=/mnt/nvme2n1/erikg/extcols-scratch
CORPUS=${1:-$BASE/mergperf-1b-snap.txt}
PFTEXT=${2:-$BASE/mergperf-1b-B/merged/frag.pftext}
OUT=$(dirname "$0")/adapter-verify-1b.results
: > "$OUT"
note() { echo "$@" | tee -a "$OUT"; }
note "### ADAPTER_VERIFY $(date -Is) corpus=$CORPUS pftext=$PFTEXT"
timeout 600 python3 - "$CORPUS" "$PFTEXT" <<'EOF' 2>&1 | tee -a "$OUT"
import sys
R = list(range(256))
R[0x01]=0xFF; R[0x02]=0xFD; R[0x03]=0xFE; R[0x04]=0xFC
R[0xFC]=0x04; R[0xFD]=0x02; R[0xFE]=0x03; R[0xFF]=0x01
RB = 1<<24
diff = 0; total = 0; remapped_bytes = 0
with open(sys.argv[1],'rb') as fc, open(sys.argv[2],'rb') as fp:
    while True:
        c = fc.read(RB); p = fp.read(RB)
        if not c and not p: break
        if len(c) != len(p):
            print("LENGTH MISMATCH"); sys.exit(1)
        remapped_bytes += sum(1 for b in c if R[b] != b)
        if c != p:
            rc = bytes(R[b] for b in c)
            if rc != p:
                diff += 1
                for k in range(len(c)):
                    if rc[k] != p[k]:
                        print(f"first mismatch at offset ~{total+k}: corpus={c[k]:02x} remapped={rc[k]:02x} pftext={p[k]:02x}")
                        break
        total += len(c)
print(f"compared {total} bytes; remap-active corpus bytes: {remapped_bytes}; mismatched blocks: {diff}")
print("ADAPTER_IDENTITY" if diff==0 else "ADAPTER_MISMATCH")
sys.exit(0 if diff==0 else 1)
EOF
rc=$?
note "rc=$rc $(date -Is)"
[ $rc -eq 0 ] && note "GATE_ADAPTER_IDENTITY_GREEN" || note "GATE_ADAPTER_IDENTITY_RED"
exit $rc
