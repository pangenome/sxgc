#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
J=bit6/sxi_logs/cross-lcp
c++ -O3 -std=c++17 -Wall -Wextra -Wpedantic "$J/measure.cpp" -o /tmp/cross-lcp-measure
/tmp/cross-lcp-measure --self-test > "$J/self-test.log"
echo "$$" > "$J/driver.pid"
/usr/bin/time -v -o "$J/measurement.time" /tmp/cross-lcp-measure \
  /home/erikg/sxgc-piletest/pile-frag.txt \
  /home/erikg/sxgc/vendor/chunk-merge-v3/chunks 16 \
  /home/erikg/sxgc/vendor/chunk-merge-v3/reference/parse.remap \
  /home/erikg/sxgc/vendor/chunk-merge-v3/reference/parse \
  1000000 20261001 131072 > "$J/measurements.jsonl" 2> "$J/progress.log"
