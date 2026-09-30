#!/usr/bin/env python3
"""Exact SXI2 v4 byte projection from banked full-source bit counts.

This does not claim an achieved output file. It gates impossible size targets
before spending hours sorting the full phi map or publishing an oversized file.
"""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[2]
source = root / "sxi_logs/sxi2-v2/size-probe.json"
rows = json.loads(source.read_text())

def align(x):
    return (x + 7) // 8 * 8

out = []
for row in rows:
    n, r, chi = (row[k] for k in ("n", "r", "chi"))
    members = row["sxi1_members"]
    l = row["elias_fano_low_bits"]
    rle = 2320 + (row["huffman_head_bits"] + 7) // 8 + (row["gamma_length_bits"] + 7) // 8
    ef = 24 + (chi * l + 7) // 8 + ((n >> l) + chi + 1 + 7) // 8
    sizes = {
        "huffman_gamma_runs": rle,
        "raw_tail_samples": members["2"]["bytes"],
        "raw_head_samples": members["3"]["bytes"],
        "anchors": members["4"]["bytes"],
        "elias_fano_chi": ef,
    }
    if "6" in members:
        sizes["names"] = members["6"]["bytes"]
    if "7" in members:
        sizes["remap"] = members["7"]["bytes"]
    sizes["phi_u_v_run"] = 24 * r
    sizes["escape"] = 0
    pos = 64 + 40 * len(sizes)
    for size in sizes.values():
        pos = align(pos) + size
    sxi1 = 64 + 40 * len(members)
    for member in members.values():
        sxi1 = align(sxi1) + member["bytes"]
    out.append({
        "artifact": pathlib.Path(row["path"]).stem,
        "n": n, "r": r, "chi": chi,
        "sxi1_bytes_from_directory": sxi1,
        "sxi2_exact_preflight_bytes": pos,
        "sxi2_over_text": pos / n,
        "sxi2_over_sxi1": pos / sxi1,
        "members": sizes,
        "gate": "FAILED" if pos >= sxi1 else "PASSED",
        "achieved": False,
    })
print(json.dumps(out, indent=2))
if "--enforce" in sys.argv and any(row["gate"] == "FAILED" for row in out):
    raise SystemExit(1)
