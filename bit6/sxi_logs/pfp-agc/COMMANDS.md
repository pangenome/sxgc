Commands are run from /tmp/sxgc-laneV unless otherwise stated.

Build (the two final flags are this host's existing static HTS dependency setup):

    tools/build_pfp_agc.sh -DCMAKE_PREFIX_PATH=/home/erikg/htsinst -DCMAKE_CXX_STANDARD_LIBRARIES=-ldeflate

Native regressions:

    python3 /tmp/pfp-agc-fork/tests/test_agc.py --pfp /tmp/pfp-agc-fork/build/pfp++ --probe /tmp/pfp-agc-fork/build/pfp_source_probe --agc /home/erikg/agc/bin/agc --agc2flat /tmp/sxi-stream/tools/agc2flat
    python3 bit6/sxi_logs/pfp-agc/check_yeast_bytes.py
    python3 bit6/sxi_logs/pfp-agc/check_hprc.py
    python3 bit6/sxi_logs/pfp-agc/run_regressions.py

Full native product build:

    XSA_PFP=/tmp/pfp-agc-fork/build/pfp++ XSA_TOOLS=/tmp/sxi-stream/tools XSA_PIPELINE=/tmp/sxgc-laneV/bit6/sxi_pipeline.py /tmp/sxi-stream/xsa-target/release/xsa build --agc /home/erikg/yeast/yeast235.agc -o /tmp/pfp-agc-gates/yeast235-native.sxi --scratch /tmp/pfp-agc-gates --log-dir /tmp/sxgc-laneV/bit6/sxi_logs/pfp-agc --threads 16 --verify-text-sample 32 --verbose

The stopped prior PID 3651283 no longer existed at completion restart. The same
full-build command was rerun with allowed r-space artifacts, logging to
`yeast-completion.log`, `yeast-completion.time`, and the stage journal
`yeast235-native-xsa-build-5najb_ty.jsonl`. No full text is materialized.

Publication byte gate (reads both SXIs in 1 MiB chunks):

    python3 bit6/sxi_logs/pfp-agc/compare_published.py

Completion regressions:

    python3 bit6/sxi_logs/pfp-agc/run_regressions.py
    /tmp/sxi-stream/tools/slim_lce_test

Both native PFP word widths were tested with `tests/test_agc.py`, substituting
`build/pfp++64` for `build/pfp++` in the native-regression command above.
Logs: `completion-native-32.log`, `completion-native-64.log`,
`completion-lce.log`, `completion-regressions.log`, and `regressions.jsonl`.

Throughput harness (bounded 100 MB oracle files, removed after each window):

    python3 bit6/sxi_logs/pfp-agc/benchmark.py

The harness waits for the owned BWT child to exit before benchmarking. On a fresh
run replace that one process-specific wait with a check that the machine is idle.
Raw source rates include the same FNV checksum in AGC and file modes; initialization
is reported separately. File mode remains serial regardless of -j, matching upstream.
