#!/usr/bin/env bash
# One new 100 MB verified stream, never rerun either member of the archived pair.
set -eu
[[ $# == 1 && $1 =~ ^[1-9][0-9]{9}$ ]] && (( $1<=1795531169 && $1>$(date +%s) )) || exit 75
cd "$(dirname "$0")/..";source gates/overlap_pin.sh;overlap_pin_series gates/overlap_bounded_shortest.sh "$1"
exec /usr/bin/python3 -BE "$PWD/gates/overlap_bounded_component.py" --shortest-100m "$1"
