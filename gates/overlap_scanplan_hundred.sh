#!/usr/bin/env bash
# Explicit operator launch only; do not chain from a smaller study.
set -euo pipefail
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] || { echo 'usage: overlap_scanplan_hundred.sh DEADLINE_UNIX' >&2; exit 64; }
(( $1 > $(date +%s) && $1 <= 1795531169 )) || { echo 'outside experiment window' >&2; exit 75; }
cd "$(dirname "$0")/.."
[[ $PWD -ef "$HOME/sxgc" ]] || { echo 'requires actual published main checkout' >&2; exit 75; }
python=/usr/bin/python3
admission=$("$python" -BE gates/overlap_scanplan_hundred.py admit "$1")
read -r rev pin <<< "$admission"
[[ $rev =~ ^[0-9a-f]{40}$ && $pin =~ ^[0-9a-f]{64}$ ]] || exit 75
snap=$(mktemp -d /tmp/scanplan-hundred-XXXXXXXX)
trap 'rm -rf -- "$snap"' EXIT
git clone --quiet --local --shared --no-checkout . "$snap"
git -C "$snap" sparse-checkout set gates docs
git -C "$snap" checkout --quiet --detach "$rev"
systemd-run --user --quiet --collect --wait --pipe -p MemoryMax=192M -p MemorySwapMax=0 -p CPUQuota=100% -p IOAccounting=yes -p Nice=10 "$python" -BE "$snap/gates/overlap_scanplan_hundred.py" worker "$1" "$pin"
