#!/usr/bin/env bash
# G3 THE LAW - adversarial fixtures for the chi layer cascade's zero-locate
# claims. Every check prints N of things examined; a vacuous check fails.
#
#   usage: chi_g3_law.sh XSA XSA_OLD WORKDIR
#
# The same-byte crossing trap (the equality width(C.b)==width(C) HOLDS while
# the law's answer differs from the raw count) is constructed twice: on an
# UNFRAMED corpus (the open-time framing verification must fail and every row
# must enumerate) and on a FRAMED corpus whose spine carries the framing byte
# (the 0x1E alarm must force the enumeration). Widths 2-4, END-adjacent,
# internal-0x1E and text-end windows are checked against the brute-force law
# oracle, and every fixture's whole row file must be BYTE-IDENTICAL to the
# PRE-CHANGE binary's.
set -uo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
XSA=${1:?new xsa}
XSA_OLD=${2:?pre-change xsa}
WORK=${3:?work dir}
mkdir -p "$WORK"

python3 "$HERE/chi_law_fixtures.py" "$XSA" "$XSA_OLD" "$WORK" 2>&1 | tee "$WORK/g3.log"
rc=${PIPESTATUS[0]}
[ $rc -eq 0 ] || { echo "GATE_FAIL: chi_g3_law exited $rc (see $WORK/g3.log)"; exit 1; }

grep -q "^CHI_G3_OK" "$WORK/g3.log" || { echo "GATE_FAIL: no CHI_G3_OK receipt"; exit 1; }
rows=$(sed -n 's/^CHI_G3_EXAMINED fixtures=\([0-9]*\) rows=\([0-9]*\).*/\2/p' "$WORK/g3.log")
fixtures=$(sed -n 's/^CHI_G3_EXAMINED fixtures=\([0-9]*\) rows=.*/\1/p' "$WORK/g3.log")
[ -n "$rows" ] && [ "$rows" -gt 0 ] || { echo "GATE_FAIL: vacuous gate - 0 rows examined"; exit 1; }
echo "G3_LAW_OK fixtures=$fixtures rows_oracled=$rows byte_parity=1 traps=2"
