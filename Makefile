# Phase targets. Requires: ragc built (../ragc/target/release),
# suffixient-array built (../suffixient-array/build), cargo on PATH.

AGC2FLAT = agc2flat/target/release/agc2flat

agc2flat/target/release/agc2flat: agc2flat/src/main.rs agc2flat/Cargo.toml
	cd agc2flat && cargo build --release

# Phase 2 smoke: 3-sample HPRC v2 end-to-end
smoke3: $(AGC2FLAT)
	$(AGC2FLAT) /home/erikg/hprcv2/smoke/hprcv2_3sample.agc -o /home/erikg/hprcv2/smoke/hprcv2_3sample.txt
	python3 tools/shard_by_contig.py /home/erikg/hprcv2/smoke/hprcv2_3sample.txt \
		/home/erikg/hprcv2/smoke/hprcv2_3sample.txt.names.tsv /home/erikg/hprcv2/smoke/shards3
	cd /home/erikg/suffixient-array/build && find /home/erikg/hprcv2/smoke/shards3 -name "*.txt" \
		! -name "*.samples.txt" | sort | xargs -P 48 -I{} sh -c \
		'python3 suffixient-array-index.py --build-index "{}" > "{}.buildlog" 2>&1'
	@echo "smoke3 done — see /home/erikg/hprcv2/smoke/shards3/"

smoke10: $(AGC2FLAT)
	$(AGC2FLAT) /home/erikg/hprcv2/smoke/hprcv2_10sample.agc -o /home/erikg/hprcv2/smoke/hprcv2_10sample.txt
	python3 tools/shard_by_contig.py /home/erikg/hprcv2/smoke/hprcv2_10sample.txt \
		/home/erikg/hprcv2/smoke/hprcv2_10sample.txt.names.tsv /home/erikg/hprcv2/smoke/shards10
	cd /home/erikg/suffixient-array/build && find /home/erikg/hprcv2/smoke/shards10 -name "*.txt" \
		! -name "*.samples.txt" | sort | xargs -P 48 -I{} sh -c \
		'python3 suffixient-array-index.py --build-index "{}" > "{}.buildlog" 2>&1'
	@echo "smoke10 done — see /home/erikg/hprcv2/smoke/shards10/"

test-mapper:
	python3 tools/mappos.py /home/erikg/yeast/yeast235.txt.names.tsv 0 316620 316621 2000000000

.PHONY: smoke3 smoke10 test-mapper
