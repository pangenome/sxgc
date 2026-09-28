import json,pathlib,resource,subprocess,os
resource.setrlimit(resource.RLIMIT_AS,(4_000_000_000,4_000_000_000))
log=pathlib.Path('bit6/sxi_logs/stream-mode');env=dict(os.environ,XSA_TOOLS='/tmp/sxi-stream/tools',RAYON_NUM_THREADS='2',CARGO_BUILD_JOBS='2')
commands=[('format-regression',['python3','bit6/test_sxi_format.py','--writer','/tmp/sxi-stream/tools/sxi_write','--xsa','/tmp/sxi-stream/xsa-target/release/xsa']),('cli-regression',['python3','bit6/test_sxi_build.py','--xsa','/tmp/sxi-stream/xsa-target/release/xsa','--log-dir',str(log)]),('phi-regression',['python3','bit6/test_phi_inverse_contract.py','/tmp/laneU/phi_inverse_heads']),('xsa-tests',['cargo','test','--release','--manifest-path','xsa/Cargo.toml','--target-dir','/tmp/sxi-stream/xsa-target']),('agc-tests',['cargo','test','--release','--manifest-path','agc2flat/Cargo.toml','--target-dir','/tmp/sxi-stream/agc-target'])]
for name,cmd in commands:
 with (log/(name+'.log')).open('w') as f:
  q=subprocess.run(['/usr/bin/time','-v','-o',str(log/(name+'.time')),*cmd],stdout=f,stderr=subprocess.STDOUT,env=env)
 print(json.dumps(dict(name=name,command=cmd,returncode=q.returncode)),flush=True)
 assert q.returncode==0
