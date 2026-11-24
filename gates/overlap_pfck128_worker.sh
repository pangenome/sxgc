#!/bin/bash
# Called by the metered driver with a sanitized environment. No stream or piece prefix exists.
set -euo pipefail
d=$1; [[ -f $d/tools.json && ! -e $d/frag.pfck2 && ! -e $d/no-product-dir && ! -e $d/stop.requested ]]
export LD_PRELOAD="$d/monitor.so" XSA_GATE_PRIVATE_STOP="$d/stop.requested" MALLOC_ARENA_MAX=2 PFCK_SPACING=128 XSA_FINISH_RSS_READINESS_ONLY=1
exec "$d/finish" --corpus "$d/raw" --remap "$d/remap" --prefix "$d/frag" --output "$d/no-product-dir/product.sxi2" --lcp-bands "$d/no-bands" --with-chi --container-stream --keep-pfck --threads 2 --wit-ram-budget 134217728 --poll-timeout 1
