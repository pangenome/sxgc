#!/usr/bin/env python3
import json,pathlib,re
root=pathlib.Path('/mnt/nvme2n1/erikg')
before=json.loads((root/'sol5d-tests/work-counters.json').read_text())
report={}
for size in ('100m','1b'):
 scratch=root/f'sol5i-gate{size}'
 merge=(scratch/'merge-v2.log').read_text();finish=(scratch/'finish-v2.log').read_text()
 line=next(x for x in merge.splitlines() if x.startswith('CROSS_KWAY '))
 after={k:int(re.search(r'\b'+k+r'=(\d+)',line)[1]) for k in ('comparisons','symbols_compared','probes')}
 corpus=next(x for x in finish.splitlines() if x.startswith('SLIM_FP pile-finish-corpus '))
 seam=next(x for x in finish.splitlines() if x.startswith('SLIM_FP pile-finish-seam '))
 report[size]=dict(before=before[size]['after']['merge'],after=after,
  percent_change={k:100*(after[k]/before[size]['after']['merge'][k]-1) for k in after},
  finish_verified_symbols=int(re.search(r'verified_symbols=(\d+)',corpus)[1]),
  finish_slim_fp=[corpus,seam],stage_work=[x for x in merge.splitlines() if x.startswith('CROSS_STAGE_WORK ')],
  merge_cross=line,gate=json.loads((root/'sol5i-tests'/f'gate-{size}.json').read_text()))
print(json.dumps(report,indent=2))
