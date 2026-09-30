#!/usr/bin/env python3
"""Small direct-SA differential for the SXI2 publish and locate path."""
import argparse
import importlib.util
import pathlib
import struct
import subprocess
import tempfile
import socket
import time
import json
import urllib.request

p = argparse.ArgumentParser()
p.add_argument("--writer", default="/tmp/sxi_write_v4")
p.add_argument("--compact-writer", default="/tmp/sxi2_write_v4")
p.add_argument("--xsa", default="xsa/target/debug/xsa")
a = p.parse_args()
spec = importlib.util.spec_from_file_location(
    "escape_codec", pathlib.Path(__file__).parent / "sxi_logs/sxi2-v3/escape_codec.py")
codec = importlib.util.module_from_spec(spec)
spec.loader.exec_module(codec)

def call(*cmd):
    return subprocess.run(list(map(str, cmd)), capture_output=True)

def http(path, pattern):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    proc = subprocess.Popen([a.xsa, "serve", "--sxi", str(path), "--bind",
                             f"127.0.0.1:{port}", "-j", "1", "--mode", "text"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        base = f"http://127.0.0.1:{port}"
        for _ in range(100):
            if proc.poll() is not None:
                raise AssertionError(proc.stderr.read())
            try:
                urllib.request.urlopen(base + "/stats", timeout=0.2).read()
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise AssertionError("serve did not listen")
        request = urllib.request.Request(base + "/query", json.dumps({"pattern":pattern}).encode(),
                                         {"Content-Type":"application/json"})
        return urllib.request.urlopen(request, timeout=5).read()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()

def fixture(root, label, text):
    n = len(text)
    sa = sorted(range(n), key=lambda i: text[i:] + text[:i])
    bwt = bytes(text[(s - 1) % n] for s in sa)
    runs = []
    for row, ch in enumerate(bwt):
        if not runs or runs[-1][0] != ch:
            runs.append([ch, row, 1])
        else:
            runs[-1][2] += 1
    r = len(runs)
    counts = [bwt.count(i) for i in range(256)]
    c = []; total = 0
    for count in counts:
        c.append(total); total += count
    width = max(1, (n - 1).bit_length())
    tails = [n - 1 - sa[start + length - 1] for _, start, length in runs]
    packed = sum(v << (i * width) for i, v in enumerate(tails))
    bits = r * width
    prefix = root / label
    ri4 = prefix.with_suffix(".ri4")
    heads = prefix.with_suffix(".heads")
    chi = prefix.with_suffix(".chi")
    old = prefix.with_suffix(".sxi")
    new = prefix.with_suffix(".sxi2")
    ri4.write_bytes(
        struct.pack("<IIQQQ256Q", 0x52585349, 4, n, 1, r, *c)
        + bytes(ch for ch, _, _ in runs)
        + b"".join(struct.pack("<I", length) for _, _, length in runs)
        + struct.pack("<QB", bits, width)
        + packed.to_bytes(((bits + 63) // 64) * 8, "little")
    )
    heads.write_bytes(b"".join(struct.pack("<Q", sa[start]) for _, start, _ in runs))
    chi.write_bytes(struct.pack("<2Q", 0, n))
    cmd = [a.writer, "--ri4", ri4, "--heads", heads, "--chi", chi, "--output", old,
           "--mode", "text"]
    if text.endswith(b"\x1e"):
        names = prefix.with_suffix(".names")
        names.write_text(f"{label}\t0\t{n-1}\n")
        cmd += ["--names", names]
    result = call(*cmd)
    assert result.returncode == 0, result.stderr
    rejected = prefix.with_suffix(".budget-rejected.sxi2")
    result = call(a.compact_writer, "--sxi", old, "--output", rejected,
                  "--validator", a.xsa, "--max-bytes", 24*r-1)
    assert result.returncode != 0 and not rejected.exists()
    edges = sorted((sa[start + length - 1], sa[runs[(i + 1) % r][1]], i)
                   for i, (_, start, length) in enumerate(runs))
    inverse = {value: row for row, value in enumerate(sa)}
    escape = {}
    gcd = __import__("math").gcd(*counts)
    for i, (u, _, run) in enumerate(edges):
        next_u = edges[(i + 1) % r][0]
        size = (next_u - u) % n or n
        if gcd != 1 and size != 1:
            escape[run] = [((u + j) % n, sa[(inverse[(u + j) % n] + 1) % n])
                           for j in range(size)]
    compact_cmd = [a.compact_writer, "--sxi", old, "--output", new, "--validator", a.xsa]
    if escape:
        sidecar = prefix.with_suffix(".escape")
        sidecar.write_bytes(codec.encode(n, r, escape))
        bad_sidecar = prefix.with_suffix(".bad-escape")
        bad_sidecar.write_bytes(sidecar.read_bytes()[:-1])
        rejected = prefix.with_suffix(".escape-rejected.sxi2")
        result = call(a.compact_writer, "--sxi", old, "--output", rejected,
                      "--validator", a.xsa, "--escape", bad_sidecar)
        assert result.returncode != 0 and not rejected.exists()
        compact_cmd += ["--escape", sidecar]
    result = call(*compact_cmd)
    assert result.returncode == 0, result.stderr
    assert old.read_bytes()[:4] == b"SXI1" and new.read_bytes()[:4] == b"SXI2"
    for path in (old, new):
        out = pathlib.Path(str(path) + ".chi-out")
        result = call(a.xsa, "sxi-info", path, "--chi-out", out)
        assert result.returncode == 0, result.stderr
        assert out.read_bytes() == struct.pack("<2Q", 0, n)
    return old, new, len(escape)

with tempfile.TemporaryDirectory(prefix="sxi2-diff-") as d:
    d = pathlib.Path(d)
    for label, text, patterns in [
        ("repeated", b"BANANABANANA\x1e", ["ANA", "BAN", "NA"]),
        ("periodic", b"ABABAB", ["AB", "BA", "A"]),
    ]:
        old, new, escaped = fixture(d, label, text)
        assert (escaped == 0) == (label == "repeated")
        saw_multi = False
        for pattern in patterns:
            args = ["mems", "--pattern", pattern, "--min-len", "1", "--mode", "text"]
            one, two = (call(a.xsa, *args, "--sxi", path) for path in (old, new))
            assert one.returncode == two.returncode == 0, (label, pattern, one.stderr, two.stderr)
            assert one.stdout == two.stdout, (label, pattern, one.stdout, two.stdout)
            saw_multi |= len(one.stdout.splitlines()) > 1
        assert saw_multi, label
        assert http(old, patterns[0]) == http(new, patterns[0]), label
print("PASS SXI1/SXI2 multi-occurrence MEM and HTTP byte parity, aperiodic phi and periodic escape, exact EF chi")
