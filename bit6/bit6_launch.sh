#!/usr/bin/env bash
# bit6_launch.sh — full-collection AGC-native .sA construction, 48-way.
# Big shards first (better tail); resumable (bit6_run_group.sh skips existing .sA).
set -u
AGC=/home/erikg/hprcv2/HPRC_r2_assemblies_0.6.1.agc
DIR=/mnt/nvme3n1/erikg/sxgc-bit6
LIST=$1   # file with one group name per line

cut -f1 "$LIST" | xargs -d '\n' -P 48 -I{} /home/erikg/sxgc/bit6/bit6_run_group.sh \
    "$AGC" "{}" "$DIR/work" "$DIR/sA" >> "$DIR/run.log" 2>&1
echo "ALL GROUPS PROCESSED $(date)" >> "$DIR/run.log"
