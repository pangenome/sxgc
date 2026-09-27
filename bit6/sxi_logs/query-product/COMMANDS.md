# Query-product reproduction

Run from `/tmp/sxgc-laneV`. No commands write to the published yeast artifacts
or the protected k10 directory. New outputs below must not already exist.

```sh
cargo build --release --manifest-path xsa/Cargo.toml
g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_write.cpp -o /tmp/sxi-query-writer
g++ -O3 -std=c++17 -Wall -Wextra bit6/sxi_text_audit.cpp -o /tmp/sxi-query-audit
g++ -O3 -std=c++17 -Wall -Wextra bit6/query_brute.cpp -o /tmp/sxi-query-brute
bash tools/build_slim_dump.sh /tmp/sxi-query-slim
python3 bit6/test_sxi_format.py --writer /tmp/sxi-query-writer --xsa xsa/target/release/xsa
python3 bit6/test_query_product.py --work /tmp/sxi-query-tests-final
python3 bit6/test_query_metadata.py
XSA_TOOLS=/tmp/sxi-query-tools python3 bit6/test_sxi_build.py --log-dir bit6/sxi_logs/query-product
XSA_TOOLS=/tmp/sxi-query-tools xsa/target/release/xsa build \
  --fasta /tmp/sxi-query-fresh/refs.fa -o /tmp/sxi-query-fresh/refs.sxi \
  --threads 2 --verify-text-sample 32 --log-dir bit6/sxi_logs/query-product --verbose
python3 bit6/test_text_audit.py
```

`/tmp/sxi-query-tools` contains symlinks to the new writer/audit and the
unchanged `/tmp/sxi-repair2/tools/{rpfbwt_endpoints,slim_dump}` and
`/tmp/laneV/tools/agc2flat`. The complete command arguments, scratch paths,
return codes, timing and RSS for fresh builds are retained in JSONL journals.
The FASTA fixture is retained at `/tmp/sxi-query-fresh/refs.fa` (three
800-base records generated with Python `random.Random(466)`).

Writer-only yeast235 rebuild from retained, previously certified construction
intermediates (this is not a new AGC/PFP build):

```sh
/tmp/sxi-query-writer \
  --ri4 /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/fresh.ri4 \
  --heads /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/fresh.head_sa \
  --chi /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/fresh.sA \
  --names /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/collection.txt.names.tsv \
  --mode dna --orientation reversed --output /tmp/sxi-query-yeast235.sxi
python3 bit6/test_query_yeast.py
python3 bit6/test_query_http.py
/tmp/sxi-query-audit \
  /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/collection.txt \
  /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/fresh.ri4 \
  /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/fresh.agg \
  /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/fresh.sA 32
xsa/target/release/xsa stats --sxi /tmp/sxi-query-yeast235.sxi
/tmp/sxi-query-slim --slim --resolve-ri4 --dict-stream \
  --ri4 /tmp/sxi-query-fresh/refs.sxi \
  --parse /tmp/sxi-query-fresh/xsa-build-6f5_1s27/parse -t 2 \
  -o /tmp/sxi-query-fresh/from-sxi.agg
cmp /tmp/sxi-query-fresh/from-sxi.agg /tmp/sxi-query-fresh/xsa-build-6f5_1s27/fresh.agg
git diff --check
git diff --cached --name-only
```

`test_query_product.py` also uses the seven retained battery containers in
`/tmp/laneV/g0-final/` and original texts in `/tmp/laneY/bat/`. Its expected
MEMs come directly from those texts, not from the index. The yeast oracle
is independent native direct-text scanning with verified rolling-hash seeds.
The test selects two 45-base reads from named records 12 and 555, including
one substitution, and verifies all MEMs of length >=20 on both strands over
the entire 3,336,986,759-byte text. Random local HTTP ports are reserved by
the tests; only server processes created by those tests are terminated.
