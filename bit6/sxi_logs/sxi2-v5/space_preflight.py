#!/usr/bin/env python3
"""Exact SXI2 v3 member projection from banked SXI1 run/chi bit counts."""
import json
from pathlib import Path

rows = json.loads((Path(__file__).parents[1] / "sxi2-v2/size-probe.json").read_text())

def align(x): return (x + 7) // 8 * 8
def bits(x): return (x + 7) // 8

out = []
for row in rows:
    n, r, chi = row["n"], row["r"], row["chi"]
    members = row["sxi1_members"]
    nw, rw = max(1, (n-1).bit_length()), max(1, (r-1).bit_length())
    ul = (n//r).bit_length()-1
    cl = row["elias_fano_low_bits"]
    sizes = {
        "1_runs": 2320 + bits(row["huffman_head_bits"]) + bits(row["gamma_length_bits"]),
        "4_old_anchors": members["4"]["bytes"],
        "5_chi_ef": 24 + bits(chi*cl) + bits((n>>cl)+chi+1),
    }
    if "6" in members: sizes["6_names"] = members["6"]["bytes"]
    if "7" in members: sizes["7_remap"] = members["7"]["bytes"]
    sizes.update({
        "8_phi_ef_intervals": 40 + bits(r*ul) + bits((n>>ul)+r+1) + bits(r*nw) + bits(r*rw),
        "9_escape": 0,
        "10_lf_move": 8 + bits(r*nw),
        "11_sparse_sa": 16 + 8*((r+1023)//1024),
    })
    projected = 64 + 40*len(sizes)
    for size in sizes.values(): projected = align(projected) + size
    old = 64 + 40*len(members)
    for member in members.values(): old = align(old) + member["bytes"]
    out.append({"artifact":Path(row["path"]).stem,"n":n,"r":r,"chi":chi,
                "sxi1_bytes":old,"sxi2_projected_bytes":projected,
                "ratio":projected/old,"phi_bits_per_run":8*sizes["8_phi_ef_intervals"]/r,
                "all_phi_and_lf_bits_per_run":8*(sizes["8_phi_ef_intervals"]+sizes["10_lf_move"])/r,
                "members":sizes,"gate":"PASSED" if projected<old else "FAILED"})
print(json.dumps(out,indent=2))
if any(x["gate"]!="PASSED" for x in out): raise SystemExit(1)
