import pathlib,sys,subprocess,os,resource,json,time
base=pathlib.Path('/mnt/nvme2n1/erikg/finish-memory-evidence'); tag=sys.argv[1]; binary=sys.argv[2]
root=base/tag; root.mkdir(); (root/'merged').mkdir(); (root/'bands-tape').mkdir()
old=sys.argv[3] if len(sys.argv)>3 else '/mnt/nvme2n1/erikg/wf-finish-as-evidence/10chi'
lines=pathlib.Path(old+'/xsa-build.log').read_text().splitlines()
env=dict(os.environ,CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='1100',CROSS_POOL_SLOTS='33554432',CROSS_BIN_CAP_SOURCE='auto')
import shutil
shutil.copy(old+'/remap-256.bin',root/'remap-256.bin')
ps=[]; records=[]
for label in ['order-tape','emit-tape','finish-v2']:
 line=next(l for l in lines if l.startswith('PHASE '+label+' START '))
 cmd=line.split(' START ')[1].split(' env=')[0].split(' rlimit_as_kb=')[0].replace(old,str(root)).split()
 if label=='finish-v2':cmd[5]=binary
 limit=int(line.split('rlimit_as_kb=')[1].split()[0])*1024
 def cap():resource.setrlimit(resource.RLIMIT_AS,(limit,limit))
 log=open(root/(label+'.log'),'w');p=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,preexec_fn=cap)
 ps.append(p);records.append(dict(label=label,pid=p.pid,command=cmd))
(root/'commands.json').write_text(json.dumps(records,indent=2))
rc=[p.wait() for p in ps];(root/'rc.json').write_text(json.dumps(rc));print(rc)
