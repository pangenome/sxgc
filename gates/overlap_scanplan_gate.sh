#!/usr/bin/env bash
set -euo pipefail
if [[ ${1:-} == 100g ]]; then echo 'REFUSED: no published scan-only admission receipt validator' >&2; exit 75; fi
[[ $# == 2 && $1 == 10g && $2 =~ ^[1-9][0-9]{9}$ ]] || { echo 'usage: overlap_scanplan_gate.sh 10g DEADLINE_UNIX' >&2; exit 64; }
(( $2 > $(date +%s) )) || { echo 'expired deadline' >&2; exit 75; }
cd "$(dirname "$0")/.."; source gates/overlap_pin.sh
overlap_pin_series gates/overlap_scanplan_gate.sh "$@"
exec systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p IOAccounting=yes -p Nice=10 /usr/bin/python3 -BE "$PWD/gates/overlap_scanplan_gate.py" "$2"
