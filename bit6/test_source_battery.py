#!/usr/bin/env python3
"""G0: actual source builds; oracles are used only for acceptance comparison."""
import argparse, filecmp, json, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--work',required=True);a=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1];work=pathlib.Path(a.work).absolute();work.mkdir(parents=True,exist_ok=True)
bat=pathlib.Path('/tmp/laneY/bat');logs=root/'bit6/sxi_logs'
for name in ['duplicates-600k','random-4-2k','random-4-20k','random-bin-20k','random-4-200k','satellite-18k','HOR-nested']:
    cmd=[root/'xsa/target/release/xsa','build','--text',bat/(name+'.txt'),'-o',work/(name+'.sxi'),
         '--scratch',work,'--verbose','--expect-heads',pathlib.Path('/tmp/laneQ/pass4')/(name+'.head_sa'),
         '--expect-ri4',bat/(name+'.ri4')]
    result=subprocess.run(list(map(str,cmd)),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    (logs/('source-battery-'+name+'.log')).write_text(result.stderr+result.stdout)
    assert result.returncode==0,(name,result.stderr)
    row=json.loads(result.stdout.splitlines()[-1]);fresh=pathlib.Path(row['work'])
    assert filecmp.cmp(fresh/'fresh.agg',bat/(name+'.agg'),shallow=False),name
    print(json.dumps(dict(name=name,status='PASS',aggregate_bytes_equal=True,**row)),flush=True)
print('G0 PASS 7/7 fresh builds, aggregate bytes and full endpoint artifacts equal')
