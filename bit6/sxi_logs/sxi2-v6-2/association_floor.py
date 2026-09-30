#!/usr/bin/env python3
"""Generic two-boundary/two-association lower bound; excludes BWT correlations."""
import json
import math
from pathlib import Path

rows=json.loads((Path(__file__).parents[1]/'sxi2-v2/size-probe.json').read_text())
out=[]
for row in rows:
    n,r=row['n'],row['r']
    permutation=math.lgamma(r+1)/math.log(2)/r
    boundary=(math.lgamma(n+1)-math.lgamma(r+1)-math.lgamma(n-r+1))/math.log(2)/r
    out.append({'artifact':Path(row['path']).stem,'one_permutation_bits_per_run':permutation,
                'one_boundary_set_bits_per_run':boundary,
                'two_permutations_two_sets_bits_per_run':2*(permutation+boundary)})
print(json.dumps(out,indent=2))
