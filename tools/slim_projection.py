#!/usr/bin/env python3
"""Conditional 466 bytes; measured constants and caveats are in SLIM_COST.md."""
import json, math
n, r = 1.403e12, 2.74e9
n10, ny = 30151407545, 3336986760
p = 333531723 / n10 * n
d = 5483754406 / n10 * n
q = 37705212 / n10 * n
items = {
    'parse': 4*p,
    'parse_sparse_boundaries': 41215978/34134006*p,
    'dict_sparse_boundaries': 3910008/3159456*q,
    'parse_fingerprints': 8*(math.floor(p/math.ceil(8*p/r))+1),
    'dict_fingerprints': 8*(math.floor(d/math.ceil(8*d/r))+1),
    'ri4_runs': 5*r,
    'ri4_starts': 8*r,
    'ri4_samples': 41*r/8,
    'LF_capacity_upper_bound': 16*r,
    'chunks_and_dict_caches': 32*65536+64*262144,
}
base = sum(items.values())
class1 = base - items['parse_fingerprints'] - items['dict_fingerprints'] + 8*p/math.ceil(p/r) + 8*d/math.ceil(d/r)
print(json.dumps(dict(n466=n, r466=r, p466_human=p,
    p466_yeast_ratio=34134006/ny*n, d466=d, dict_phrases466=q,
    tau1=math.ceil(8*p/r), tau2=math.ceil(8*d/r), bytes=items,
    base_GB=base/1e9, reserve_GB=10, projected_peak_GB=base/1e9+10,
    resident_dict_peak_GB=(base+d)/1e9+10,
    class1_tau_peak_GB=class1/1e9+10), indent=2))
