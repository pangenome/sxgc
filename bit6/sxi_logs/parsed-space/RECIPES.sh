#!/usr/bin/env bash
# Review recipes. Run from a durable checkout, in this order, with fresh paths.
# Do not start an hours-long gate in a worktree scheduled for automatic reaping.
set -euo pipefail
case_name=${1:?usage: RECIPES.sh build|a|b|c|d}
root=$(git rev-parse --show-toplevel)
journal="$root/bit6/sxi_logs/parsed-space"
pfp="$journal/pfp-bounded-build/pfp-external"
rpf="$journal/rpf-final-build/rpfbwt-external"
# Pick a new suffix per invocation. Existing work/scratch paths fail closed.
run_id=${RUN_ID:-manual-$(date -u +%Y%m%dT%H%M%SZ)}
work="$journal/$case_name-$run_id"
scratch="/mnt/nvme2n1/erikg/sxgc-parsed-space-$case_name-$run_id"
case "$case_name" in
  build)
    python3 tools/build_external_frontend.py pfp /tmp/pfp-agc-fork /tmp/pfp-agc-fork/build "$journal/pfp-bounded-build"
    python3 tools/build_external_frontend.py rpfbwt /tmp/sxgc-dist-build-02/.build/deps /tmp/sxgc-dist-build-02/.build/rpfbwt-build "$journal/rpf-final-build"
    ;;
  a)
    SXI_DICT_CACHE_BYTES=536870912 python3 tools/gate_external_frontend.py \
      --agc --input /home/erikg/yeast/yeast235.agc --reference /tmp/rpfbwt-64-yeast/parse \
      --work "$work" --scratch "$scratch" --pfp "$pfp" --rpfbwt "$rpf" --memory-bytes 3500000000
    ;;
  b)
    # Read-only retained parse is copied to new work. Never use work-main.
    # No historical tail reference exists; this compares BWT, metadata, heads.
    SXI_DICT_CACHE_BYTES=8589934592 SXI_SA_BLOCK_BYTES=1073741824 \
      python3 tools/gate_external_frontend.py \
      --retained-parse /mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp \
      --reference /mnt/nvme3n1/erikg/sxgc-pilot/k10/h10ss_pfp \
      --work "$work" --scratch "$scratch" --pfp "$pfp" --rpfbwt "$rpf" --memory-bytes 32000000000
    # Complete the four-file oracle only after the supervisor publishes the
    # independent tail reference. Do not read/write its live job directories.
    ;;
  c)
    # Measurement corpus only: 229 bytes <=5 and 13,301,465 bytes >=128 were
    # replaced with 6, along with existing 0x1e; a unique 0x1e is appended.
    # Replace with the integrated byte
    # remap lane for an exact arbitrary-byte input gate.
    mkdir -p "$journal/fixtures"
    fragment_input="$journal/fixtures/pile-frag-terminal-cleaned.txt"
    if [[ ! -e "$fragment_input" ]]; then
      fragment_input="$journal/fixtures/pile-frag-$run_id.cleaned.txt"
      python3 tools/prepare_external_measurement.py --source /home/erikg/sxgc-piletest/pile-frag.txt \
        --raw-copy "$journal/fixtures/pile-frag-$run_id.txt" --cleaned "$fragment_input" \
        --manifest "$journal/fixtures/pile-cleaning-$run_id.json"
    fi
    SXI_DICT_CACHE_BYTES=2147483648 SXI_SA_BLOCK_BYTES=134217728 \
      python3 tools/gate_external_frontend.py \
      --input "$fragment_input" --terminal 1e \
      --work "$work" --scratch "$scratch" --pfp "$pfp" --rpfbwt "$rpf" --memory-bytes 64000000000 \
      --through-sxi --tools /home/erikg/sxgc/sealed-tools --xsa /home/erikg/sxgc/xsa/target/release/xsa
    ;;
  d)
    # CONDITIONAL: do not execute until a-c pass and LF run-ID overflow has
    # been resolved or a measured R<2^32 bound is established. Use 10 GB first;
    # the present LF arrays narrow run IDs and cannot safely support 50 GB
    # under the measured run-density scenario.
    mkdir -p "$journal/fixtures"
    # Prepared measurement slice only, pending the byte-remap integration.
    # No raw-text temporary is created by the construction stages.
    slice_input="$journal/fixtures/pile-10gb-$run_id.cleaned.txt"
    python3 tools/prepare_external_measurement.py --source /mnt/nvme2n1/erikg/pile.txt --limit 10000000000 \
      --cleaned "$slice_input" --manifest "$journal/fixtures/pile-10gb-$run_id.cleaning.json"
    SXI_DICT_CACHE_BYTES=17179869184 SXI_SA_BLOCK_BYTES=1073741824 \
      python3 tools/gate_external_frontend.py \
      --input "$slice_input" --terminal 1e --work "$work" --scratch "$scratch" \
      --pfp "$pfp" --rpfbwt "$rpf" --memory-bytes 400000000000 \
      --through-sxi --tools /home/erikg/sxgc/sealed-tools --xsa /home/erikg/sxgc/xsa/target/release/xsa
    ;;
  *) echo "unknown gate" >&2; exit 2 ;;
esac
