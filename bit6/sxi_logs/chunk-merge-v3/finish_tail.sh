#!/usr/bin/env bash
set -euo pipefail
cd /home/erikg/sxgc
J=bit6/sxi_logs/chunk-merge-v3
bash "$J/finish_gate.sh" > "$J/finish-gate.log" 2>&1
echo "FINISH_GATE_PASS"
python3 "$J/summarize.py" > "$J/summarize.log" 2>&1 && cat "$J/../chunk-merge-v3/TABLE.md" 2>/dev/null || true
tail -2 "$J/finish-gate.log"
