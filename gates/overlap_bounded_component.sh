#!/usr/bin/env bash
set -eu
[[ $# == 1 && $1 =~ ^[1-9][0-9]{0,10}$ ]] || { echo 'usage: overlap_bounded_component.sh DEADLINE' >&2; exit 64; }
(( $1 <= 1795531169 && $1 > $(date +%s) )) || { echo 'outside experiment window' >&2;exit 75; }
cd "$(dirname "$0")/..";source gates/overlap_pin.sh;overlap_pin_series gates/overlap_bounded_component.sh "$1"
exec /usr/bin/python3 -BE "$PWD/gates/overlap_bounded_component.py" "$1"
