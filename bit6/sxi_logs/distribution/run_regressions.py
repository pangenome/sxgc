#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys
root=pathlib.Path('/tmp/sxgc-laneV');prefix=pathlib.Path('/tmp/sxgc-dist-final');logs=root/'bit6/sxi_logs/distribution'
env={k:v for k,v in os.environ.items() if not k.startswith('XSA_')}
env.update(XSA_TOOLS=str(prefix), XSA_PIPELINE=str(root/'bit6/sxi_pipeline.py'))
cases=[
 ('manifest-tests',['python3','bit6/test_tool_manifest.py']),
 ('distribution-tests',['python3','bit6/test_distribution.py','--prefix',str(prefix),'--text','/tmp/laneY/bat/random-4-2k.txt','--heads','/tmp/laneQ/pass4/random-4-2k.head_sa','--ri4','/tmp/laneY/bat/random-4-2k.ri4','--log-dir',str(logs/'final')]),
 ('build-regression',['python3','bit6/test_sxi_build.py','--xsa',str(prefix/'xsa'),'--log-dir',str(logs/'regressions')]),
 ('format-regression',['python3','bit6/test_sxi_format.py','--xsa',str(prefix/'xsa'),'--writer',str(prefix/'sxi_write')]),
 ('phi-regression',['python3','bit6/test_phi_inverse_contract.py',str(prefix/'phi_inverse_heads')]),
 ('idempotence',['bash','tools/build_all.sh',str(prefix)]),
 ('reference-manifest',['python3','tools/tool_manifest.py','verify','--prefix',str(prefix),'--manifest','tools/MANIFEST.sha256']),
]
results=[]
for name,cmd in cases:
 with (logs/(name+'.log')).open('w') as f:
  result=subprocess.run(cmd,cwd=root,env=env,stdout=f,stderr=subprocess.STDOUT)
 results.append(dict(name=name,command=cmd,returncode=result.returncode))
 print(name,result.returncode,flush=True)
(logs/'regressions.json').write_text(json.dumps(results,indent=2)+'\n')
sys.exit(any(x['returncode'] for x in results))
