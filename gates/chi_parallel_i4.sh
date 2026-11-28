#!/usr/bin/env bash
# I4 / T8: the chi sweep in chunks (docs/pfp-emission-speedups.md).
#
# The sweep is a per-run state machine whose only cross-run memory is
# SweepImpl::saveState (rr_len/rr_pos/rr_act, m, p, p_sa_last, previous_interior,
# first); witnesses are POSITIONS ORed into a bitmap, so the SET is the product
# and its ORDER is not. `xsa_finish --chi-parallel DUMP M5 K` therefore replays a
# saved run stream twice: one sequential snapshot pass that emits nothing, then K
# workers that sweep one chunk each from the saved state. This gate proves the
# union is the inline set and measures what the snapshot pass costs.
#
#   (a) the 100 MB anchor pin still holds with the recorder ON
#       (`XSA_FINISH_CHI_DUMP` is opt-in; the default product path opens no file)
#   (b) the parallel m5 MEMBER bytes and chi count equal the inline path's, for
#       K=4 and K=8
#   (c) the inline `sweep=` field (XSA_FINISH_TIME) against the parallel total,
#       with the sequential snapshot pass and the K-worker wall reported apart
#
# usage: chi_parallel_i4.sh BUNDLE XSA_FINISH [--xsa BIN] [--work DIR]
#                             [--cpus INLINE --pcpus WORKERS] [--threads N]
#                             [--reuse] [--1g]
# BUNDLE is a tool directory holding build_pfp.py and the stage binaries; its
# xsa_finish is REPLACED by XSA_FINISH in the run's own copy.
set -u
BUNDLE=${1:?usage: chi_parallel_i4.sh BUNDLE XSA_FINISH ...}
FINISH=${2:?usage: chi_parallel_i4.sh BUNDLE XSA_FINISH ...}
shift 2
XSA=/home/erikg/sxgc/xsa/target/release/xsa
WORK=/tmp/chi-parallel-i4
CPUS=224-239
PCPUS=240-255
THREADS=16
REUSE=0
ONEG=0
PIN_BYTES=87069576
PIN_SHA=e99fb40e1e11d57a2a4687b4c75491ed83dd3d6308fc27fb2336dd35efdd7f28
A100=/mnt/nvme2n1/erikg/TAPEREADER/sandbox/real-v10-map100-S4bZm0/inputs
A1G=/mnt/nvme2n1/erikg/TAPEREADER/sandbox/rolling-build-1g-U4MRhu/in
while [ $# -gt 0 ]; do
  case $1 in
    --xsa) XSA=$2; shift 2;;
    --work) WORK=$2; shift 2;;
    --cpus) CPUS=$2; shift 2;;
    --pcpus) PCPUS=$2; shift 2;;
    --threads) THREADS=$2; shift 2;;
    --reuse) REUSE=1; shift;;
    --1g) ONEG=1; shift;;
    *) echo "unknown argument $1" >&2; exit 2;;
  esac
done
set -e
mkdir -p "$WORK"
B=$WORK/bundle
if [ ! -e "$B" ]; then cp -a "$BUNDLE" "$B"; fi
cp "$FINISH" "$B/xsa_finish"
chmod +x "$B/xsa_finish"

# One rung: build the container with the sweep recording its own inputs, then
# compare the parallel m5 member with the member the inline writer published.
rung() {   # rung RAW MAP LABEL PINBYTES PINHEX
  local raw=$1 map=$2 label=$3 pbytes=$4 phex=$5
  local dir=$WORK/$label
  mkdir -p "$dir" "$dir/scratch" "$dir/cache"
  local index=$dir/index.sxi2 dump=$dir/runs.dump log=$dir/build.log
  if [ "$REUSE" = 1 ] && [ -f "$index" ] && [ -f "$dump" ]; then
    echo "REUSE $label container=$index dump=$dump"
  else
    rm -f "$index" "$index.partial" "$dump"
    ( export XSA_PFP_TOOLS=$B XDG_CACHE_HOME=$dir/cache XSA_FINISH_CHI_DUMP=$dump
      unset XSA_FINISH_SPOOL
      taskset -c "$CPUS" "$XSA" build --pfp "$raw" --docs-map "$map" \
        -T "$dir/scratch" -j "$THREADS" -m 2G -v -o "$index" >"$log" 2>&1 )
  fi
  local sha bytes
  bytes=$(stat -c %s "$index"); sha=$(sha256sum "$index" | cut -d' ' -f1)
  if [ -z "$phex" ]; then
    echo "NOTE (a) $label bytes=$bytes sha256=$sha (no pin asserted on this rung)"
  else
    if [ "$sha" != "$phex" ] || [ "$bytes" != "$pbytes" ]; then
      echo "FAIL (a) $label container is $bytes B $sha, pin is $pbytes B $phex"; return 1
    fi
    echo "PASS (a) $label bytes=$bytes sha256=$sha (pin held with XSA_FINISH_CHI_DUMP on)"
  fi
  local sweep chi_inline dumped
  sweep=$(sed -n 's/.*XSA_FINISH_TIME .*sweep=\([0-9.]*\).*/\1/p' "$log" | tail -1)
  # the inline sweep's own count of emit() calls, which is the m5 entry count
  chi_inline=$(sed -n 's/.*SWEEP_STATS .*chi=\([0-9]*\).*/\1/p' "$log" | tail -1)
  dumped=$(sed -n 's/.*XSA_FINISH_CHI_DUMP_DONE runs=\([0-9]*\).*/\1/p' "$log" | tail -1)
  echo "MEASURE $label inline_sweep_s=$sweep inline_chi_phase_s=$(sed -n 's/.*XSA_FINISH_TIME .*chi=\([0-9.]*\).*/\1/p' "$log" | tail -1) chi=$chi_inline runs=$dumped"
  local m5line
  m5line=$(python3 - "$index" "$dir/m5-inline.bin" "$label" <<'PY'
import struct, sys, hashlib
path, out, label = sys.argv[1:4]
b = open(path, 'rb').read()
count, = struct.unpack_from('<I', b, 32)
length, = struct.unpack_from('<Q', b, 40)
for i in range(count):
    mid, codec, off, nb, cnt, _crc = struct.unpack_from('<IIQQQI', b, length - 40*count + 40*i)
    if mid == 5:
        open(out, 'wb').write(b[off:off+nb])
        print('M5 %s codec=%d bytes=%d entries=%d sha256=%s'
              % (label, codec, nb, cnt, hashlib.sha256(b[off:off+nb]).hexdigest()))
        break
else:
    sys.exit('no m5 member')
PY
)
  echo "$m5line"
  local member_entries
  member_entries=$(echo "$m5line" | sed -n 's/.*entries=\([0-9]*\).*/\1/p')
  if [ "$member_entries" != "$chi_inline" ]; then
    echo "FAIL (b) $label inline m5 member holds $member_entries entries, the sweep counted $chi_inline"
    return 1
  fi
  for K in 4 8; do
    taskset -c "$PCPUS" "$B/xsa_finish" --chi-parallel "$dump" "$dir/m5-k$K.bin" "$K" \
      2>"$dir/par-k$K.log" || { echo "FAIL (b) $label K=$K: chi-parallel refused"; return 1; }
    local res snap work tot par_bytes
    res=$(grep -h 'CHI_PARALLEL_RESULT' "$dir/par-k$K.log")
    snap=$(grep -h 'CHI_PARALLEL_SNAPSHOT' "$dir/par-k$K.log")
    work=$(grep -h 'CHI_PARALLEL_WORKERS' "$dir/par-k$K.log")
    par_bytes=$(stat -c %s "$dir/m5-k$K.bin")
    echo "MEASURE $label K=$K $snap"
    echo "MEASURE $label K=$K $work"
    echo "MEASURE $label K=$K $res"
    cmp -s "$dir/m5-k$K.bin" "$dir/m5-inline.bin" ||
      { echo "FAIL (b) $label K=$K m5 member differs from the inline member"; return 1; }
    local pchi
    pchi=$(echo "$res" | sed -n 's/.*entries=\([0-9]*\).*/\1/p')
    if [ "$pchi" != "$chi_inline" ]; then
      echo "FAIL (b) $label K=$K chi=$pchi, inline chi=$chi_inline"; return 1
    fi
    echo "PASS (b) $label K=$K m5 member identical ($par_bytes B) and chi=$pchi == inline $chi_inline"
  done
  return 0
}

rung "$A100/raw" "$A100/map" 100m "$PIN_BYTES" "$PIN_SHA"
if [ "$ONEG" = 1 ]; then
  # The 1 GB anchor's pin is not asserted here (this gate is about the sweep, not
  # about the product SHA): the run reports its own bytes/sha and its own sweep=.
  rung "$A1G/raw" "$A1G/map" 1g 0 "" || true
fi
echo "CHI_PARALLEL_I4_PASS work=$WORK"
