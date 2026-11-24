#!/usr/bin/env bash
set -eu
[[ $# == 1 && $1 =~ ^[1-9][0-9]{0,10}$ ]] || { echo 'usage: overlap_bounded_component.sh DEADLINE' >&2; exit 64; }
exec /usr/bin/python3 -E "$(realpath "$(dirname "$0")/overlap_bounded_component.py")" "$1"
