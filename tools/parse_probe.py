#!/usr/bin/env python3
"""parse_probe.py — the UNIFIED compressibility parameter for position
resolution: the PFP parse length.  Replaces the handwavy periodic/duplicate
taxonomy: my gap_probe 'non-jumpable' number was really 'non-PERIODIC under
the only test implemented' — duplicates were never unjumpable, the probe
was too weak.  The parse covers BOTH degenerate families with one mechanism
(phrase-occurrence coordinates).  NOTE: alphabet must avoid bytes <= 5
(pfp++ reserves them); battery uses 0x0b..0x0e (order-preserving shift,
so r/gaps/periods are identical to the 1..4 battery)."""
import contextlib
import io
import os
import random
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) or ".")
from gap_probe import analyze

PFP = "/home/erikg/pfp/build/pfp++"


def gen():
    rng = random.Random(20261001)
    yield "satellite-18k", b"\x0b\x0c\x0d" * 6000 + bytes(rng.choice((0x0b, 0x0c, 0x0d, 0x0e)) for _ in range(500))
    yield "HOR-nested", (b"\x0b\x0c\x0d" * 100 + b"\x0e") * 60 + bytes(rng.choice((0x0b, 0x0c, 0x0d, 0x0e)) for _ in range(300))
    paras = [bytes(rng.choice((0x0b, 0x0c, 0x0d, 0x0e)) for _ in range(300)) for _ in range(40)]
    docs = []
    for _ in range(50):
        for p in rng.sample(paras, len(paras)):
            docs.append(p)
    yield "duplicates-600k", b"".join(docs)[:600000]
    parts = []
    for i in range(400):
        parts.append(paras[rng.randrange(40)] if rng.random() < 0.5
                     else bytes(rng.choice((0x0b, 0x0c, 0x0d, 0x0e)) for _ in range(300)))
    yield "dup+unique-120k", b"".join(parts)
    yield "random-20k", bytes(rng.choice((0x0b, 0x0c, 0x0d, 0x0e)) for _ in range(20000))


if __name__ == "__main__":
    os.makedirs("/tmp/pfp_bat", exist_ok=True)
    os.chdir("/tmp/pfp_bat")
    print(f"{'text':18s} {'n':>7s} {'r':>7s} {'parse':>7s} {'=r':>6s} {'nonper':>8s}")
    for name, x in gen():
        open(f"{name}.txt", "wb").write(x)
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            r, nonper, tot = analyze(name, x)
        subprocess.run([PFP, "-t", f"{name}.txt", "-o", f"{name}_pfp", "-w", "10", "-p", "100"],
                       capture_output=True, text=True)
        plen = os.path.getsize(f"{name}_pfp.parse") // 4 if os.path.exists(f"{name}_pfp.parse") else None
        print(f"{name:18s} {len(x):7d} {r:7d} {str(plen):>7s} "
              f"{(plen / r if plen and r else 0):6.2f} {nonper:8d}")
    print("PARSE PROBE DONE")
