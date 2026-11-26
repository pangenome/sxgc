#!/bin/sh
# One metered PFP arm inside a user cgroup scope on a physical CPU set.
# usage: pfp_scale.sh CPUS MEMORYMAX CPUQUOTA XSA RAW MAP OUTDIR LABEL THREADS GRANT EXPECTED_SHA [SCRATCH_LIMIT]
set -eu
cpus=$1 memmax=$2 quota=$3 xsa=$4 raw=$5 map=$6 out=$7 label=$8 threads=$9 grant=${10} expect=${11} limit=${12:-0}
exec systemd-run --user --quiet --scope --collect \
  -p "MemoryMax=$memmax" -p MemorySwapMax=0 -p "CPUQuota=$quota" \
  taskset -c "$cpus" python3 -B "$(dirname "$0")/pfp_scale_arm.py" \
  "$xsa" "$raw" "$map" "$out" --label "$label" --threads "$threads" --memory "$grant" \
  --expected-sha "$expect" --scratch-limit "$limit"
