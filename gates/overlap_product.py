"""Sandbox-only external triples -> existing cutter/LCP slices -> certified finish.
Run in one shared cgroup, producer / pipe sink / finish concurrent. No stored triples.
"""
import hashlib,json,os,pathlib,subprocess,sys,time
# Historical payload below is preserved, not a safe launcher: the old sink's
# flat directory scan skipped frag.pieces/p-*. Refuse before inputs or mkdir.
print('HOLD: under-counted nested run pieces; see docs/overlap_finish_credits.md',file=sys.stderr)
raise SystemExit(75)

P=pathlib.Path;D,T,repo=map(P,sys.argv[1:4]);I=D/'in';S=P('/mnt/nvme2n1/erikg/TAPEREADER/sandbox'); n=(I/'corpus').stat().st_size; assert n==100000503
reserve=int(os.environ['XSA_GATE_SINK_RESERVE']); (D/'merged').mkdir(); (D/'bands').mkdir();pref=D/'merged/frag';env=dict(os.environ,XSA_TEST_EXTERNAL_TRIPLES='1',SS_LCP_SLICE_WINDOW='8',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='128');common=['--corpus',I/'corpus','--remap',S/'remap-256.bin','--threads','2'];cg=P('/sys/fs/cgroup')/P('/proc/self/cgroup').read_text().strip().split('::/')[1];start=time.monotonic();peak=0;stop=False
with (D/'producer.log').open('xb') as pl,(D/'sink.log').open('xb') as sl,(D/'finish.log').open('xb') as fl:
 a=subprocess.Popen([sys.executable,repo/'gates/overlap_all_sets.py',D,T],stdout=subprocess.PIPE,stderr=pl,env=env)
 b=subprocess.Popen([repo/'xsa/target/release/pfp_sort_r',*common,'--out-prefix',pref,'--bands',D/'bands','--scratch-budget-bytes',str(reserve)],stdin=a.stdout,stdout=sl,stderr=sl,env=env);a.stdout.close()
 c=subprocess.Popen(['/tmp/xsa_finish_cur',*common,'--prefix',pref,'--lcp-bands',D/'bands','--output',D/'product.sxi2','--poll-timeout','0','--wit-ram-budget','134217728','--with-chi','--container-stream'],stdout=fl,stderr=fl,env=dict(env,LD_PRELOAD=str(T/'monitor.so'),XSA_GATE_PRIVATE_STOP=str(D/'stop.requested')))
 while True:
  allocated=0
  for root,_,files in os.walk(D):
   for f in files:
    try:allocated+=(P(root)/f).stat().st_blocks*512
    except FileNotFoundError:pass
  peak=max(peak,allocated);rc=[w.poll() for w in (a,b,c)]
  if allocated>800*2**20 or any(x not in (0,None) for x in rc):
   stop=True;(D/'stop.requested').touch();md=D/'merged/modeb';
   if md.exists():(md/'stop.requested').touch()
  if None not in rc:break
  time.sleep(.5)
sha=hashlib.file_digest((D/'product.sxi2').open('rb'),'sha256').hexdigest() if rc==[0,0,0] else None
receipt=dict(rc=rc,stop=stop,sha256=sha,wall_s=time.monotonic()-start,shared_peak=int((cg/'memory.peak').read_text()),whole_arm_peak=peak,sink_reserve=reserve);(D/'product.json').write_text(json.dumps(receipt,indent=2));print(receipt,flush=True);assert not stop and sha=='6325c843c0d9aa9be00d69260aafdd794936e2e79f16174efb00c43db831f341'
