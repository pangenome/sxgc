import os,pathlib,subprocess,json,tempfile,time
logs=pathlib.Path(__file__).resolve().parent
base=pathlib.Path(tempfile.mkdtemp(prefix='from-zero-',dir='/mnt/nvme3n1/erikg/sxgc-k10sep'))
env=dict(os.environ,XSA_TOOLS='/tmp/k10-endpoints-debug/tools',XSA_PIPELINE='/tmp/sxgc-laneV/bit6/sxi_pipeline.py')
xsa='/tmp/k10-endpoints-debug/tools/xsa'
source='/mnt/nvme3n1/erikg/sxgc-k10sep/k10sep.txt'
manifest=dict(base=str(base),source=source,started=time.time(),runs=[])
manifest_path=logs/'from-zero-manifest.json'
def save(): manifest_path.write_text(json.dumps(manifest,indent=2)+'\n')
save()
control=None
for label in ('A','B'):
    scratch=base/('scratch-'+label);scratch.mkdir()
    assert not list(scratch.iterdir())
    output=base/('k10-control.sxi' if label=='A' else 'k10.sxi')
    command=[xsa,'build','--text',source,'-o',str(output),'--threads','16','--scratch',str(scratch),'--log-dir',str(logs),'--verbose']
    if label=='B':command+=['--expect-ri4',str(control/'fresh.ri4'),'--expect-heads',str(control/'fresh.head_sa')]
    row=dict(label=label,command=command,output=str(output),scratch=str(scratch),started=time.time());manifest['runs'].append(row);save()
    with (logs/('from-zero-'+label+'.log')).open('w') as log:
        result=subprocess.Popen(command,stdout=log,stderr=log,env=env)
        heartbeat=0
        while result.poll() is None:
            try: result.wait(timeout=5)
            except subprocess.TimeoutExpired: pass
            now=time.time()
            if now-heartbeat>=55:
                heartbeat=now
                journals=list(logs.glob(output.stem+'-xsa-build-*.jsonl'))
                if journals:
                    records=[json.loads(line) for line in journals[-1].read_text().splitlines()]
                    latest=records[-1]
                    print(json.dumps(dict(update='from-zero',run=label,elapsed_seconds=round(now-row['started']),stage=latest.get('stage'),stage_elapsed_seconds=round(now-latest['start']) if 'start' in latest else None,returncode=latest.get('returncode'),peak_rss_kib=latest.get('peak_rss_kib'))),flush=True)
                else: print(json.dumps(dict(update='from-zero',run=label,status='starting')),flush=True)
    row.update(returncode=result.returncode,finished=time.time());save()
    if result.returncode:raise SystemExit(result.returncode)
    work=list(scratch.glob('xsa-build-*'));assert len(work)==1
    row['work']=str(work[0]);save()
    if label=='A':control=work[0]
manifest['status']='PASS';save()
print(json.dumps(manifest),flush=True)
