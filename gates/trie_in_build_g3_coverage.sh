#!/usr/bin/env bash
# T4: coverage/latency of the IN-BUILD trie on the 10 GB container, 1,000 random
# 8-byte contexts sampled from the corpus (no corpus read during the query: the
# contexts are drawn once, written to a batch file, and the container is the only
# input of the measurement).
#
# usage: gates/trie_in_build_g3_coverage.sh <container> [contexts]
set -u
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
X=${X:-$ROOT/xsa/target/release/xsa}
C=${1:?usage: g3_coverage.sh <container> [n]}
K=${2:-1000}
CORPUS=${CORPUS:-/mnt/nvme2n1/erikg/pf-10gb.txt}
D=${D:-/mnt/nvme2n1/erikg/trie-in-build/g10}
B=$D/ctx8.bin
mkdir -p "$D"
echo "G3_BOX $(uptime)"
echo "G3_CONTAINER $C $(stat -c%s "$C") bytes"
# A deterministic sample (xorshift64 seeded 20261110), 8 bytes each, 0x1e-separated.
seed=$((20261110)); span=$((10000000000 - 8))
: > "$B"
for ((i = 0; i < K; i++)); do
  seed=$(( (seed ^ (seed << 13)) & 0xFFFFFFFFFFFF )); seed=$(( seed ^ (seed >> 7) ))
  seed=$(( (seed ^ (seed << 17)) & 0xFFFFFFFFFFFF ))
  off=$(( seed % span ))
  dd if="$CORPUS" bs=1 skip="$off" count=8 status=none >> "$B"
  printf '\x1e' >> "$B"
done
echo "G3_SAMPLE contexts=$K bytes=$(stat -c%s "$B") seed=20261110 sample=8B"
sha256sum "$B"
"$X" branch-of "$C" --batch "$B" | tee "$D/coverage.txt"
# the same measurement on the post-hoc container, for the side-by-side
if [ -f "${REF_CONTAINER:-/mnt/nvme2n1/erikg/ctx-trie/g10-d8.sxi2}" ]; then
  "$X" branch-of "${REF_CONTAINER:-/mnt/nvme2n1/erikg/ctx-trie/g10-d8.sxi2}" --batch "$B" | tee "$D/coverage-posthoc.txt"
fi
