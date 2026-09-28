#!/usr/bin/env bash
set -euo pipefail
cd /tmp/sxgc-laneV
while kill -0 3656477 2>/dev/null; do sleep 5; done
tools/build_pfp_agc.sh -DCMAKE_PREFIX_PATH=/home/erikg/htsinst -DCMAKE_CXX_STANDARD_LIBRARIES=-ldeflate > bit6/sxi_logs/pfp-agc/build-script.log 2>&1
mkdir -p /tmp/pfp-agc-gates/traced
/usr/bin/time -v -o bit6/sxi_logs/pfp-agc/traced-parse.time strace -f -yy -s 0 -e trace=write,pwrite64,writev,pwritev,openat,close,lseek,ftruncate,truncate,mmap,msync,rename,unlink -o bit6/sxi_logs/pfp-agc/reader-write.trace /tmp/pfp-agc-fork/build/pfp++ -t /home/erikg/yeast/yeast235.agc --agc --agc-names /tmp/pfp-agc-gates/traced/names.tsv -o /tmp/pfp-agc-gates/traced/parse -w 10 -p 100 -j 16 --tmp-dir /tmp/pfp-agc-gates/traced > bit6/sxi_logs/pfp-agc/traced-parse.log 2>&1
cmp /tmp/pfp-agc-gates/traced/parse.parse /tmp/sxi-stream/yeast/xsa-build-gpvh5hug/parse.parse
cmp /tmp/pfp-agc-gates/traced/parse.dict /tmp/sxi-stream/yeast/xsa-build-gpvh5hug/parse.dict
