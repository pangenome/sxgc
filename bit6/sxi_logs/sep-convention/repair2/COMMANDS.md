# Reproduction commands used in this lane

Run from `/tmp/sxgc-laneV`. Outputs must be fresh paths; the pipeline refuses
clobbering an existing SXI. Full per-stage argv is also in each JSONL journal.

```sh
export XSA_TOOLS=/tmp/sxi-repair2/tools
export XSA_PIPELINE=/tmp/sxgc-laneV/bit6/sxi_pipeline.py
export XSA_AGC2FLAT=/tmp/sxi-sep-8xat6poa/agc-target/release/agc2flat
export RAYON_NUM_THREADS=16
bash tools/build_sxi_tools.sh /tmp/sxi-repair2/tools
# Final bounded fingerprint probe change was rebuilt atomically:
bash tools/build_slim_dump.sh /tmp/sxi-repair2/tools/rpfbwt_endpoints.next bit6/rpfbwt_endpoints.cpp
mv /tmp/sxi-repair2/tools/rpfbwt_endpoints.next /tmp/sxi-repair2/tools/rpfbwt_endpoints
bash tools/build_slim_dump.sh /tmp/sxi-repair2/tools/slim_dump.next
mv /tmp/sxi-repair2/tools/slim_dump.next /tmp/sxi-repair2/tools/slim_dump

python3 bit6/test_sxi_separator.py --xsa xsa/target/release/xsa --agc2flat "$XSA_AGC2FLAT" --reserved-agc /tmp/sxi-sep-8xat6poa/reserved.agc --adapter "$XSA_TOOLS/rpfbwt_endpoints" --work /tmp/sxi-repair2/gates --log-dir bit6/sxi_logs/sep-convention/repair2/gates
python3 bit6/test_seam_repair.py --tools "$XSA_TOOLS" --xsa xsa/target/release/xsa --work /tmp/sxi-repair2/exhaustive-final
python3 bit6/test_sxi_build.py --log-dir bit6/sxi_logs/sep-convention/repair2/cli-final
python3 bit6/test_sxi_format.py --writer "$XSA_TOOLS/sxi_write"

# Read-only reuse of earlier AGC producer artifacts for adapter preflight only:
/usr/bin/time -v -o bit6/sxi_logs/sep-convention/repair2/yeast235-preflight.time "$XSA_TOOLS/rpfbwt_endpoints" /tmp/sxi-repair-o4aGmN/yeast235/xsa-build-kfclgrq6/parse /tmp/sxi-repair2/yeast235-preflight.ri4 /tmp/sxi-repair2/yeast235-preflight.heads 1e

# Full fresh builds, no producer artifact reuse:
xsa/target/release/xsa build --agc /home/erikg/yeast/yeast235.agc -o /tmp/sxi-repair2/yeast235.sxi --threads 16 --scratch /tmp/sxi-repair2/yeast235 --log-dir bit6/sxi_logs/sep-convention/repair2 --verbose
xsa/target/release/xsa build --text /mnt/nvme3n1/erikg/sxgc-yeast/grl/yeast_pfp2.txt -o /mnt/nvme3n1/erikg/sxi-repair2/yeast.sxi --threads 16 --scratch /mnt/nvme3n1/erikg/sxi-repair2 --log-dir bit6/sxi_logs/sep-convention/repair2 --expect-chi 85404240 --verbose
xsa/target/release/xsa build --text /tmp/sxi-repair2/yeast235/xsa-build-26al38ee/collection.txt -o /tmp/sxi-repair2/yeast235-control.sxi --threads 16 --scratch /tmp/sxi-repair2/yeast235-control --log-dir bit6/sxi_logs/sep-convention/repair2 --expect-heads /tmp/sxi-repair2/yeast235-preflight.heads --expect-ri4 /tmp/sxi-repair2/yeast235-preflight.ri4 --verbose

python3 bit6/sxi_logs/sep-convention/repair2/audit_publications.py
git diff --check
git diff --cached --name-only
```

The preflight predates the final polylog query cap. The fresh publications
use the final adapter. The old CLI regression initially expected the previous
certificate error; its assertion was updated to require the precise new
policy refusal, and the final run passes. All earlier logs are retained.
