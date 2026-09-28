import subprocess,json,pathlib,os
root=pathlib.Path('/tmp/sxgc-laneV');logs=root/'bit6/sxi_logs/pfp-agc'
env=dict(os.environ,XSA_PFP='/tmp/pfp-agc-fork/build/pfp++',XSA_TOOLS='/tmp/sxi-stream/tools',XSA_PIPELINE=str(root/'bit6/sxi_pipeline.py'))
cases=[('format',['python3','bit6/test_sxi_format.py','--writer','/tmp/sxi-stream/tools/sxi_write','--xsa','/tmp/sxi-stream/xsa-target/release/xsa']),('cli',['python3','bit6/test_sxi_build.py','--xsa','/tmp/sxi-stream/xsa-target/release/xsa','--log-dir',str(logs)]),('phi',['python3','bit6/test_phi_inverse_contract.py','/tmp/laneU/phi_inverse_heads']),('cargo-xsa',['cargo','test','--release','--manifest-path','xsa/Cargo.toml','--target-dir','/tmp/sxi-stream/xsa-target']),('cargo-agc',['cargo','test','--release','--manifest-path','agc2flat/Cargo.toml','--target-dir','/tmp/sxi-stream/agc-target'])]
with (logs/'regressions.jsonl').open('w') as records:
 for name,cmd in cases:
  with (logs/f'regression-{name}.log').open('w') as f:q=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT)
  result=dict(name=name,command=cmd,returncode=q.returncode);records.write(json.dumps(result)+'\n');records.flush();print(result,flush=True)
