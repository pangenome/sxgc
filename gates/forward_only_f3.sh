#!/usr/bin/env bash
# F3 - THE OLD CONTAINER IS REFUSED (forward-only gate).
#
# Every command that can be pointed at a container must refuse the pre-m15
# artifact BY NAME, print the rebuild instruction, and NOT advertise any escape
# hatch (no --legacy-*, no "query-legacy", no "use ... instead").
#
#   usage: gates/forward_only_f3.sh BINARY OLD_CONTAINER CORPUS OUTDIR
set -u
X=${1:?binary}
OLD=${2:?old container}
CORPUS=${3:?corpus}
D=${4:?outdir}
mkdir -p "$D"

echo "F3_BOX $(uptime)"
echo "F3_BINARY $X $(sha256sum "$X" | cut -d' ' -f1)"
echo "F3_OLD $(ls -l "$OLD")"
echo "F3_OLD_HEADER $(xxd -l 8 -p "$OLD")"

printf '{"doc": 0, "start": 16, "len": 16, "left": 64}\n{"doc": 1, "start": 16, "len": 16, "left": 64}\n' > "$D/w.jsonl"
printf 'the ' > "$D/pat.txt"
printf '\x1e\x1e' > "$D/ctx.bin"
printf 'the corpus text of a query window, 200 bytes of it\n' > "$D/query.txt"
python3 -c "import struct,sys; open(sys.argv[1],'wb').write(b''.join(struct.pack('<Q', 64*(i+1)) for i in range(4)))" "$D/pos.bin"

check() {
  name=$1
  shift
  setsid timeout --kill-after=10 600 "$@" > "$D/$name.out" 2> "$D/$name.err"
  rc=$?
  msg=$(cat "$D/$name.out" "$D/$name.err")
  echo "F3_CMD name=$name rc=$rc"
  echo "$msg" | sed 's/^/    | /'
  [ $rc -ne 0 ] || { echo "F3_FAIL name=$name rc=0 (a refusal must not exit 0)"; return 1; }
  echo "$msg" | grep -q "SXI2 v6" || { echo "F3_FAIL name=$name: the message does not name the format"; return 1; }
  echo "$msg" | grep -q 'rebuild with `xsa build`' || { echo "F3_FAIL name=$name: the message does not carry the rebuild instruction"; return 1; }
  if echo "$msg" | grep -Eiq 'legacy|--ms\b|instead|fall ?back|v6 lite'; then
    echo "F3_FAIL name=$name: the message mentions a legacy escape hatch"; return 1
  fi
  echo "F3_PASS name=$name"
}

fails=0
check query    "$X" query "$OLD" --patterns "$D/pat.txt" --count
check consult  "$X" consult --container "$OLD" --windows "$D/w.jsonl" --output "$D/consult.rows" --corpus "$CORPUS"
check seeds    "$X" seeds "$OLD" "$D/query.txt"
check search   "$X" search "$OLD" --pattern "the "
check branches "$X" branches "$OLD" --contexts "$D/ctx.bin" --corpus "$CORPUS"
check mem      "$X" mem "$OLD" "the "
check stats    "$X" stats --sxi "$OLD"

echo "F3_COMMANDS 7"
echo "F3_DONE fails=$fails"
