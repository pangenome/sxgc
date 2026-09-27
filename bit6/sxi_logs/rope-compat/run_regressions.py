import json, pathlib, resource, subprocess, os
resource.setrlimit(resource.RLIMIT_AS, (9<<30,9<<30))
log=pathlib.Path('bit6/sxi_logs/rope-compat')
commands=[
 ('native-regression',['python3','bit6/test_query_product.py','--work','/tmp/sxi-rope-native-regression']),
 ('format-regression',['python3','bit6/test_sxi_format.py','--writer','/tmp/sxi-query-writer','--xsa','xsa/target/release/xsa']),
 ('build-regression',['python3','bit6/test_sxi_build.py','--log-dir',str(log/'build-regression')]),
 ('phi-regression',['python3','bit6/test_phi_inverse_contract.py','/tmp/laneU/phi_inverse_heads']),
 ('rust-tests',['cargo','test','--release','--manifest-path','xsa/Cargo.toml'])]
for label,cmd in commands:
 with (log/(label+'.log')).open('wb') as out:
  p=subprocess.run(['/usr/bin/time','-v','-o',str(log/(label+'.time')),*cmd],stdout=out,stderr=subprocess.STDOUT,env=dict(os.environ,XSA_TOOLS='/tmp/sxi-query-tools'))
 print(json.dumps(dict(command=cmd,exit=p.returncode,label=label)),flush=True)
 assert p.returncode==0
