#!/usr/bin/env bash
# bit6_run_group.sh — AGC-native construction of ONE contig group's queryable .sA
# usage: bit6_run_group.sh <AGC> <contig> <workdir> <outdir>
#   extract (temp) -> pscan + streamed -A components -> build_store_sA_index -o agc
#   -> .sA + sidecar + .agc-ref persisted to outdir; temp deleted.
# Resumable: skipped if outdir/<contig>.txt.sA exists.
set -e
export LD_LIBRARY_PATH=/home/erikg/sxgc/ragc-ffi/target/release:$LD_LIBRARY_PATH
AGC=$1; CONTIG=$2; WORK=$3; OUT=$4
A2F=/home/erikg/sxgc/agc2flat/target/release/agc2flat
SAB=/home/erikg/suffixient-array/build

[ -f "$OUT/$CONTIG.txt.sA" ] && { echo "SKIP $CONTIG (exists)"; exit 0; }
mkdir -p "$WORK" "$OUT"

$A2F "$AGC" --group "$CONTIG" -o "$WORK/$CONTIG.txt" 2> "$WORK/$CONTIG.extract.log"
LEN=$(stat -c%s "$WORK/$CONTIG.txt")
N=$((LEN + 1))

# streamed components (Bit 4): .suff/.lcs/.mult (0-based positions)
# tiny texts need a small window (w=10 throws length_error; w>=4 required by pscan)
W=10; [ "$LEN" -lt 10000 ] && W=4
$SAB/pfp-src/pscan "$WORK/$CONTIG.txt" -w $W -p 100 -t 4 -s > "$WORK/$CONTIG.pscan.log" 2>&1
$SAB/suff-set-src/pfp_suffixient -i "$WORK/$CONTIG.txt" -w $W -n $N -A -o "$WORK/$CONTIG.txt" \
    > "$WORK/$CONTIG.scan.log" 2>&1

# queryable .sA with the AGC itself as the oracle (Bit 5) — no oracle disk
cp "$WORK/$CONTIG.txt" "$WORK/$CONTIG.txt" 2>/dev/null || true
printf '%s\n' "$AGC" > "$WORK/$CONTIG.txt.agc-ref"
$SAB/sA-index-src/build_store_sA_index -i "$WORK/$CONTIG.txt" -t sA -o agc -l 30 \
    > "$WORK/$CONTIG.build.log" 2>&1

mv "$WORK/$CONTIG.txt.sA" "$OUT/$CONTIG.txt.sA"
cp "$WORK/$CONTIG.names.tsv" "$OUT/$CONTIG.names.tsv"
cp "$WORK/$CONTIG.txt.agc-ref" "$OUT/$CONTIG.txt.agc-ref"
rm -f "$WORK/$CONTIG.txt" "$WORK/$CONTIG.names.tsv" "$WORK/$CONTIG.txt.agc-ref" \
      "$WORK/$CONTIG.txt.suff" "$WORK/$CONTIG.txt.lcs" "$WORK/$CONTIG.txt.mult" \
      "$WORK/$CONTIG.txt.alph"* "$WORK/$CONTIG.txt.sA" \
      "$WORK/$CONTIG.txt.parse"* "$WORK/$CONTIG.txt.dict" "$WORK/$CONTIG.txt.occ" \
      "$WORK/$CONTIG.txt.last"* "$WORK/$CONTIG.txt.ilist" "$WORK/$CONTIG.txt.sai"* \
      "$WORK/$CONTIG.txt.bwsai*"
echo "DONE $CONTIG chi+components -> .sA $(stat -c%s "$OUT/$CONTIG.txt.sA") B"
