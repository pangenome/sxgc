"""Compare all retained yeast artifacts with the accepted identity build."""
from pathlib import Path
import json,subprocess
root=Path(__file__).resolve().parents[3];log=root/'bit6/sxi_logs/byte-remap'
records=[json.loads(s) for s in next(log.glob('yeast235-xsa-build-*.jsonl')).read_text().splitlines()]
assert records[-1].get('status')=='PASS','yeast build has not passed'
fresh=Path(records[-1]['work']);base=Path('/tmp/pfp-agc-gates/xsa-build-5najb_ty')
assert (fresh/'parse.remap').read_bytes()==bytes(range(256))
pairs=[(fresh/p,base/p) for p in ['parse.dict','parse.parse','parse.parse.dict','parse.parse.parse','collection.txt.names.tsv','fresh.head_sa','fresh.ri4','fresh.agg','fresh.sA']]
pairs.append((Path(records[-1]['output']),Path('/tmp/pfp-agc-gates/yeast235-native.sxi')))
results=[]
for a,b in pairs:
 cmd=['cmp',str(a),str(b)];r=subprocess.run(cmd,capture_output=True)
 results.append({'command':cmd,'returncode':r.returncode,'bytes':a.stat().st_size,'stderr':r.stderr.decode()})
(log/'yeast-identity.json').write_text(json.dumps({'chi':(fresh/'fresh.sA').stat().st_size//8,'identity_table':True,'comparisons':results},indent=2)+'\n')
assert all(r['returncode']==0 for r in results),results
print('PASS yeast235: parse/dictionaries/names/heads/RI4/aggregates/chi/SXI byte-identical; chi='+str((fresh/'fresh.sA').stat().st_size//8))
