# Phase targets.
#
# External dependencies (set to your local checkouts/builds):
#   RAGC            github.com/ekg/ragc               (built: cargo build --release)
#   SUFFIXIENT      github.com/regindex/suffixient-array (built: cmake + make)
#
# Data defaults point at local HPRC v2 smoke collections; override via env.

RAGC            ?= $(HOME)/ragc
SUFFIXIENT      ?= $(HOME)/suffixient-array/build
HPRCV2          ?= $(HOME)/hprcv2
PARALLEL        ?= 48

AGC2FLAT = agc2flat/target/release/agc2flat

agc2flat/target/release/agc2flat: agc2flat/src/main.rs agc2flat/Cargo.toml
	cd agc2flat && cargo build --release

# Phase 2 smoke: 3-sample HPRC v2 end-to-end
smoke3: $(AGC2FLAT)
	$(AGC2FLAT) $(HPRCV2)/smoke/hprcv2_3sample.agc -o $(HPRCV2)/smoke/hprcv2_3sample.txt
	python3 tools/shard_by_contig.py $(HPRCV2)/smoke/hprcv2_3sample.txt \
		$(HPRCV2)/smoke/hprcv2_3sample.txt.names.tsv $(HPRCV2)/smoke/shards3
	cd $(SUFFIXIENT) && find $(HPRCV2)/smoke/shards3 -name "*.txt" \
		! -name "*.samples.txt" | sort | xargs -P $(PARALLEL) -I{} sh -c \
		'python3 suffixient-array-index.py --build-index "{}" > "{}.buildlog" 2>&1'
	@echo "smoke3 done — see $(HPRCV2)/smoke/shards3/"

smoke10: $(AGC2FLAT)
	$(AGC2FLAT) $(HPRCV2)/smoke/hprcv2_10sample.agc -o $(HPRCV2)/smoke/hprcv2_10sample.txt
	python3 tools/shard_by_contig.py $(HPRCV2)/smoke/hprcv2_10sample.txt \
		$(HPRCV2)/smoke/hprcv2_10sample.txt.names.tsv $(HPRCV2)/smoke/shards10
	cd $(SUFFIXIENT) && find $(HPRCV2)/smoke/shards10 -name "*.txt" \
		! -name "*.samples.txt" | sort | xargs -P $(PARALLEL) -I{} sh -c \
		'python3 suffixient-array-index.py --build-index "{}" > "{}.buildlog" 2>&1'
	@echo "smoke10 done — see $(HPRCV2)/smoke/shards10/"

test-mapper:
	python3 tools/mappos.py $(HPRCV2)/smoke/hprcv2_3sample.txt.names.tsv 0

.PHONY: smoke3 smoke10 test-mapper
