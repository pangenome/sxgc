#!/usr/bin/env python3
"""Preflight the proposed *independently stored* phi interval permutation.

This is an information budget for that representation, not a lower bound on
all indexes: a different index may derive the permutation from the BWT at
query time.  Two sorted endpoint lists alone lose the interval pairing.
"""
import json
import math
from pathlib import Path

HERE = Path(__file__).resolve().parent
V5 = HERE.parent / "sxi2-v5" / "space-preflight.json"
FRAGMENT_TARGET_BYTES = 1_300_000_000
MAX_MOVE_BITS_PER_RUN = 12


def log2_factorial(count):
    return math.lgamma(count + 1) / math.log(2)


def log2_choose(universe, count):
    return (math.lgamma(universe + 1) - math.lgamma(count + 1)
            - math.lgamma(universe - count + 1)) / math.log(2)


def calculate(row):
    n, r = row["n"], row["r"]
    members = row["members"]
    retained = {key: value for key, value in members.items()
                if key not in ("8_phi_ef_intervals", "9_escape", "10_lf_move")}
    # Any explicit association of r source intervals with r destination
    # intervals has r! possibilities. The source interval starts are a subset
    # of the n positions. This intentionally omits the second endpoint list,
    # select directories, checksums, and alignment, so it is optimistic.
    pairing_bits = log2_factorial(r)
    source_bits = log2_choose(n, r)
    retained_bytes = sum(retained.values())
    floor_bytes = retained_bytes + math.ceil((pairing_bits + source_bits) / 8)
    return {
        "artifact": row["artifact"], "n": n, "r": r, "chi": row["chi"],
        "sxi1_bytes": row["sxi1_bytes"],
        "v5_bytes": row["sxi2_projected_bytes"],
        "retained_members_bytes": retained,
        "pairing_bits_per_run": pairing_bits / r,
        "source_boundary_bits_per_run": source_bits / r,
        "move_form_optimistic_bits_per_run": (pairing_bits + source_bits) / r,
        "optimistic_total_bytes": floor_bytes,
        "optimistic_below_v5": floor_bytes < row["sxi2_projected_bytes"],
        "v5_below_sxi1": row["sxi2_projected_bytes"] < row["sxi1_bytes"],
        "few_bits_gate": pairing_bits / r <= MAX_MOVE_BITS_PER_RUN,
        "fragment_target_gate": (floor_bytes <= FRAGMENT_TARGET_BYTES
                                 if row["artifact"] == "pile-frag" else None),
    }


def main():
    rows = [calculate(row) for row in json.loads(V5.read_text())]
    report = {
        "model": "explicit independently stored phi interval pairing",
        "caveat": "Not a lower bound on an index deriving pairing from the BWT; no v6 codec is projected or published.",
        "target_max_move_bits_per_run": MAX_MOVE_BITS_PER_RUN,
        "fragment_target_max_bytes": FRAGMENT_TARGET_BYTES,
        "rows": rows,
        "gate": "FAILED" if any(not row["few_bits_gate"] or
            row["fragment_target_gate"] is False for row in rows) else "PASSED",
    }
    print(json.dumps(report, indent=2))
    return 0 if report["gate"] == "PASSED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
