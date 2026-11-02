#!/usr/bin/env bash
# J1/J3/J4: real 150 MB builds, independent document oracle, failure retention.
# Existing 150 MB fixture/oracle is unchanged; sidecars use each record's first
# line as its title. OUT and DOCS must not already exist.
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
XSA=${XSA:-$ROOT/xsa/target/release/xsa}
CORPUS=${CORPUS:-/dev/shm/t150m.txt}
OUT=${OUT:?set OUT to a fresh evidence directory}
DOCS=$OUT/docs
mkdir -p "$OUT"
python3 "$ROOT/tools/binbwt/gate_query_docs.py" prepare "$CORPUS" "$DOCS"
"$XSA" build --input "$CORPUS" --scratch "$OUT/success" --tape \
    --threads 16 --memory-gb 16 --timeout 900 --chi-expect 44034230 >"$OUT/success.log" 2>&1
C=$OUT/success/$(basename "${CORPUS%.*}").sxi2
export C XSA
bash "$ROOT/tools/binbwt/gate_qlite_150m.sh" >"$OUT/oracle.log" 2>&1
"$XSA" query "$C" --patterns-fasta "$ROOT/tools/binbwt/pats.fa" -j 16 \
    >"$OUT/plain.tsv" 2>"$OUT/plain.log"
"$XSA" query "$C" --patterns-fasta "$ROOT/tools/binbwt/pats.fa" -j 16 --docs "$DOCS" \
    >"$OUT/docs.tsv" 2>"$OUT/docs.log"
python3 "$ROOT/tools/binbwt/gate_query_docs.py" check "$DOCS" "$OUT/plain.tsv" "$OUT/docs.tsv" | tee "$OUT/docs-oracle.log"
# Mismatched corpus metadata warns and falls back to plain offsets.
mkdir "$OUT/wrong-docs"
cp "$DOCS"/{seps.bin,titles.bin,MANIFEST.json} "$OUT/wrong-docs/"
python3 - "$OUT/wrong-docs/MANIFEST.json" <<'PYNEG'
import json, pathlib, sys
path = pathlib.Path(sys.argv[1])
manifest = json.loads(path.read_text())
manifest['doc_count'] += 1
path.write_text(json.dumps(manifest))
PYNEG
"$XSA" query "$C" --patterns-fasta "$ROOT/tools/binbwt/pats.fa" --docs "$OUT/wrong-docs" \
    >"$OUT/mismatch.tsv" 2>"$OUT/mismatch.log"
grep -q 'corpus metadata mismatch' "$OUT/mismatch.log"
cmp "$OUT/plain.tsv" "$OUT/mismatch.tsv"
"$XSA" query "$C" --patterns-fasta "$ROOT/tools/binbwt/pats.fa" --docs "$DOCS" --output jsonl \
    >"$OUT/mode.jsonl" 2>"$OUT/mode.log"
python3 - "$OUT/plain.tsv" "$OUT/mode.jsonl" <<'PYJSON'
import json, pathlib, sys
expected = sorted(tuple(map(int, row.split())) for row in pathlib.Path(sys.argv[1]).read_text().splitlines())
actual = []
for line in pathlib.Path(sys.argv[2]).read_text().splitlines():
    row = json.loads(line)
    assert row['count'] == len(row['offsets'])
    actual.extend((row['pattern_id'], pos) for pos in row['offsets'])
assert sorted(actual) == expected
PYJSON
# Force a native finish failure: the known chi is 44034230, not 1.
if "$XSA" build --input "$CORPUS" --scratch "$OUT/failure" --tape \
    --threads 16 --memory-gb 16 --timeout 900 --chi-expect 1 >"$OUT/failure.log" 2>&1; then
    echo 'FAIL: deliberately incorrect chi expectation succeeded'; exit 1
fi
python3 - "$OUT" <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
success = root/'success'
files = sorted(p.name for p in success.iterdir())
assert len([p for p in files if p.endswith('.sxi2')]) == 1
assert all(p.is_file() and p.suffix in ('.sxi2', '.log', '.time') for p in success.iterdir()), files
log = (success/'xsa-build.log').read_text()
assert re.search(r'^SCRATCH_CLEANED bands_bytes=\d+ waves_bytes=\d+$', log, re.M)
assert 'SCRATCH_RETAINED' not in log
failure = root/'failure'
log = (failure/'xsa-build.log').read_text()
assert len(re.findall(r'^SCRATCH_RETAINED bands_bytes=\d+ reason=failure$', log, re.M)) == 1
assert 'SCRATCH_CLEANED' not in log
assert (failure/'bands-tape').is_dir() and (failure/'waves-tape').is_dir()
assert any(p.is_file() and p.stat().st_size for p in (failure/'bands-tape').rglob('*'))
print('J4a SUCCESS only_container_and_provenance=' + ','.join(files))
print('J4b FAILURE scratch_retained=1 journal_lines=1')
PY
cat "$OUT/oracle.log"
echo 'GATES J1 J3 J4 PASS'
