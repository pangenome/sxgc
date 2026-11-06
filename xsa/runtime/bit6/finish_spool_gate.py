#!/usr/bin/env python3
"""Native finish gate; sorter/emitter stay alive while the finish restarts.

Usage: finish_spool_gate.py STAGE_DIRECTORY CORPUS NEW_GATE_DIRECTORY
The directory must contain scatter_sort, cross_lcp_merge and xsa_finish.
No subprocess deadline is installed. All logs and spools are retained.
"""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import time

stages, corpus, root = map(Path, sys.argv[1:])
stages, corpus, root = stages.resolve(), corpus.resolve(), root.resolve()
root.mkdir(parents=True, exist_ok=False)
threads = os.environ.get("SPOOL_GATE_THREADS", "16")
chi_flag = "--no-chi" if os.environ.get("SPOOL_GATE_CHI") == "0" else "--with-chi"

def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()

def case(name, interrupt):
    dest = root / name
    dest.mkdir()
    for folder in ("merged", "bands"):
        (dest / folder).mkdir()
    journal = dest / "checkpoint.journal"
    journal.touch()
    subprocess.run([stages / "xsa_finish", "--corpus", corpus,
                    "--sentinel-remap", dest / "remap.bin"], check=True,
                   stdout=subprocess.DEVNULL, stderr=(dest / "remap.log").open("wb"))
    env = dict(os.environ, CROSS_V2_PIECES="1", CROSS_V2_PIECE_WINDOW="1100",
               CROSS_POOL_SLOTS="2097152", XSA_TAPE_WAIT_S="inf", XSA_CHECKPOINT_JOURNAL=str(journal))
    env.pop("XSA_KILL_AT", None)
    env.pop("XSA_FINISH_SPOOL", None)
    def start(cmd, log, child_env):
        with (dest / log).open("wb") as f:
            return subprocess.Popen(list(map(str, cmd)), stdout=f,
                                    stderr=subprocess.STDOUT, env=child_env)
    order = start([stages / "scatter_sort", "--corpus", corpus, "--remap", dest / "remap.bin",
                   "--scratch", dest / "waves", "--tape-bands", dest / "bands",
                   "--pending-bands", "8", "--budget-bytes", "12884901888",
                   "--memory-gb", "24", "--threads", threads], "order.log", env)
    emit = start([stages / "cross_lcp_merge", "--binbwt", corpus, "--remap", dest / "remap.bin",
                  "--out-prefix", dest / "merged/frag", "--threads", threads,
                  "--tape-bands", dest / "bands"], "emit.log", env)
    cmd = [stages / "xsa_finish", "--prefix", dest / "merged/frag", "--corpus", corpus,
           "--remap", dest / "remap.bin", "--output", dest / "out.sxi2",
           "--threads", threads, "--poll-timeout", "0", chi_flag, "--lcp-bands", dest / "bands"]
    def finish(log, child_env):
        child = start(cmd, log, child_env)
        while child.poll() is None:
            for label, peer in (("sorter", order), ("emitter", emit)):
                rc = peer.poll()
                if rc not in (None, 0):
                    child.terminate()
                    child.wait()
                    raise RuntimeError(f"{label} failed with {rc}; see {dest}")
            time.sleep(.05)
        return child.returncode
    finish_env = dict(env)
    if interrupt:
        finish_env.update(XSA_FINISH_SPOOL="1", XSA_KILL_AT="after_fsync_before_unlink")
    try:
        rc = finish("finish-first.log", finish_env)
        assert rc == (86 if interrupt else 0), f"finish returned {rc}; see {dest}"
        if interrupt:
            finish_env.pop("XSA_KILL_AT")
            assert finish("finish-resume.log", finish_env) == 0, dest
        assert order.wait() == 0, dest
        assert emit.wait() == 0, dest
    except BaseException:
        for child in (order, emit):
            if child.poll() is None:
                child.terminate()
        for child in (order, emit):
            child.wait()
        raise
    return dest, cmd, finish_env, start

clean, *_ = case("clean", False)
expected = sha(clean / "out.sxi2")
print(f"(a) clean_sha256={expected}", flush=True)
resumed, cmd, env, start = case("interrupted", True)
actual = sha(resumed / "out.sxi2")
assert actual == expected, (expected, actual)
print(f"(b) clean_sha256={expected} resumed_sha256={actual} byte_identical=1", flush=True)
text = (resumed / "finish-resume.log").read_text()
state = re.search(r"SPOOL_STATE blocks_present=(\d+) blocks_missing=(\d+) resumed_from=(\S+)", text)
work = re.search(r"WORK_AVOIDED phase=finish done=(\d+) total=(\d+) ratio=([\d.]+)", text)
assert state and int(state[1]) > 0 and state[3] != "none", state
assert work and float(work[3]) < .9, work
print("(c) " + state[0], flush=True)
print(work[0], flush=True)
spools = resumed / "merged/frag.finish-spool"
blocks = sorted(spools.glob("*.entropy"))
deleted = blocks[len(blocks)//2]
deleted.unlink()
print(f"(d) deleted={deleted.name}", flush=True)
assert start(cmd, "finish-hole.log", env).wait() == 0, resumed
hole = (resumed / "finish-hole.log").read_text()
assert len(re.findall(r"SPOOL_RECOMPUTE ", hole)) == 1, hole[-2000:]
assert "blocks_missing=1" in hole, hole[-2000:]
assert sha(resumed / "out.sxi2") == expected
for line in hole.splitlines():
    if line.startswith(("SPOOL_RECOMPUTE ", "SPOOL_STATE ", "WORK_AVOIDED ")):
        print(line, flush=True)
print(f"(d) repaired_sha256={sha(resumed / 'out.sxi2')} byte_identical=1 recomputed_blocks=1", flush=True)
print("NATIVE_FINISH_GATE_PASS", flush=True)
