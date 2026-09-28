#!/usr/bin/env python3
import filecmp,json,os,pathlib,subprocess,tempfile
root=pathlib.Path('/tmp/sxgc-laneV');prefix=pathlib.Path('/tmp/sxgc-dist-final');logs=root/'bit6/sxi_logs/distribution/full-battery';logs.mkdir(exist_ok=True)
work=pathlib.Path(tempfile.mkdtemp(prefix='sxi-distribution-battery-'));bat=pathlib.Path('/tmp/laneY/bat')
env={k:v for k,v in os.environ.items() if not k.startswith('XSA_')};env['XSA_PIPELINE']=str(root/'bit6/sxi_pipeline.py')
results=[]
for name in ['duplicates-600k','random-4-2k','random-4-20k','random-bin-20k','random-4-200k','satellite-18k','HOR-nested']:
 cmd=[prefix/'xsa','build','--text',bat/(name+'.txt'),'-o',work/(name+'.sxi'),'--scratch',work,'--threads','4','--verbose','--log-dir',logs,'--expect-heads',pathlib.Path('/tmp/laneQ/pass4')/(name+'.head_sa'),'--expect-ri4',bat/(name+'.ri4')]
 p=subprocess.run(list(map(str,cmd)),env=env,capture_output=True,text=True)
 (logs/(name+'.log')).write_text(p.stdout+p.stderr)
 assert p.returncode==0,(name,p.stderr)
 row=json.loads(p.stdout.splitlines()[-1]);fresh=pathlib.Path(row['work'])
 assert filecmp.cmp(fresh/'fresh.agg',bat/(name+'.agg'),shallow=False),name
 result=dict(name=name,status='PASS',aggregate_bytes_equal=True,**row);results.append(result);print(json.dumps(result),flush=True)
(logs/'results.json').write_text(json.dumps(results,indent=2)+'\n')
