import pathlib, subprocess, json, resource
resource.setrlimit(resource.RLIMIT_AS,(149_000_000_000,149_000_000_000))
logs=pathlib.Path(__file__).resolve().parent
root=pathlib.Path('/tmp/k10-endpoints-debug')
for label,src,terminal in [('yeast','/mnt/nvme3n1/erikg/sxi-repair2/xsa-build-t2tghkm7','0a'),('yeast235','/tmp/sxi-repair2/yeast235/xsa-build-26al38ee','1e')]:
    src=pathlib.Path(src); work=root/label; work.mkdir()
    for path in src.glob('parse.*'):
        subprocess.run(['cp','--reflink=auto',str(path),str(work/path.name)],check=True)
    command=['/usr/bin/time','-v','-o',str(logs/(label+'.time')),str(root/'tools/rpfbwt_endpoints'),str(work/'parse'),str(work/'fresh.ri4'),str(work/'fresh.head_sa'),terminal]
    with (logs/(label+'.log')).open('w') as log:
        subprocess.run(command,stdout=log,stderr=log,check=True)
    for name in ('fresh.ri4','fresh.head_sa'):
        subprocess.run(['cmp',str(work/name),str(src/name)],check=True)
    print(json.dumps(dict(regression=label,endpoint_bytes='identical',command=command)),flush=True)
