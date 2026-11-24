"""Run INSIDE a dedicated user systemd cgroup; no global cache or memory changes.
Usage: systemd-run --user --wait --pipe --collect -p MemoryMax=512M -p MemorySwapMax=0 -p CPUQuota=200% -p Nice=10 python3 /abs/path/cache_pipeline.py physical 512
Disk: producer counted scratch limit + sampled whole-directory cooperative stop (not a filesystem quota).
"""
import argparse, hashlib, json, os, pathlib, re, subprocess, time
P = pathlib.Path
ap = argparse.ArgumentParser(); ap.add_argument('order', choices=['dictionary','physical']); ap.add_argument('mib',type=int); a=ap.parse_args()
cg=P('/sys/fs/cgroup')/P('/proc/self/cgroup').read_text().strip().split('::/')[1]
assert int((cg/'memory.max').read_text()) == a.mib*2**20 and int((cg/'memory.swap.max').read_text()) == 0
s=P('/mnt/nvme2n1/erikg/TAPEREADER/sandbox'); r=s/f'private-pipeline-{a.order}-{time.time_ns()}'
r.mkdir(); (r/'merged').mkdir(); (r/'bands').mkdir(); print('receipt='+str(r),flush=True)
corpus=r/'corpus'; subprocess.run(['cp','--reflink=auto',s/'pile-slice100.bin',corpus],check=True)
from private_cold import cold
cold(corpus)
start=time.monotonic(); prefix=r/'merged/frag'; common=['--corpus',corpus,'--remap',s/'remap-256.bin','--threads','2']; env=dict(os.environ,XSA_SCAN_PHYSICAL=str(int(a.order=='physical')),XSA_SA_CEILING='2000000',XSA_BAND_WORKSPACE_BYTES='512000000',SS_LCP_SLICE_WINDOW='128',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='128')
# Sample /proc IO by worker, plus actual cgroup refaults/pressure/peak. Counts are cumulative.
def fields(p):
    return {k.rstrip(':'):int(v) for k,v in (line.split() for line in p.read_text().splitlines())}
finish_env=dict(os.environ);stop_test=env.get('XSA_GATE_CANCEL_TEST')=='1'; requested=None
if 'XSA_GATE_MONITOR_SO' in env:
    so=P(env['XSA_GATE_MONITOR_SO']).resolve(strict=True);finish_env.update(LD_PRELOAD=str(so),XSA_GATE_PRIVATE_STOP=str(r/'stop.requested'))
assert not stop_test or 'XSA_GATE_MONITOR_SO' in env
workers=[];peakdisk=0;last='startup';counters={};disk_limit=800*2**20
read_guard=int(os.environ.get('XSA_TEST_READ_GUARD','0')); over_reads=False
with (r/'run.log').open('wb') as pl,(r/'finish.log').open('wb') as fl,(r/'samples.jsonl').open('w',1) as rec:
    binary=P(__file__).resolve().parents[1]/'xsa/target/release/pfp_sort_r'
    workers.append(subprocess.Popen([binary,*common,'--out-prefix',prefix,'--bands',r/'bands','--budget-bytes','64000000000','--scratch-budget-bytes','200000000'],stdout=pl,stderr=pl,env=env))
    workers.append(subprocess.Popen(['/tmp/xsa_finish_cur',*common,'--prefix',prefix,'--lcp-bands',r/'bands','--output',r/'product.sxi2','--poll-timeout','0','--wit-ram-budget','134217728','--with-chi','--container-stream'],stdout=fl,stderr=fl,env=finish_env))
    with (r/'run.log').open() as progress:
        while True:
            for line in progress:
                if line.startswith('SS_OVERLAP_PHASE '): last=line.strip()
                elif line.startswith('SS_') and not (env.get('XSA_TEST_OVERLAP_BINNED') and line.startswith('SS_SCRATCH ')): last=line.strip().split()[0]
            for label,w in zip(['producer','finish'],workers):
                try:
                    io=fields(P(f'/proc/{w.pid}/io')); counters[label]=[io['read_bytes'],io['write_bytes']]
                # Linux may return EACCES during task exit before waitpid reports it.
                # I/O is sampled telemetry; don't tear down a still-finalizing peer.
                except (FileNotFoundError, ProcessLookupError, PermissionError): pass
            allocated=0
            for d,_,files in os.walk(r):
                for f in files:
                    try: allocated+=(P(d)/f).stat().st_blocks*512
                    except FileNotFoundError: pass
            peakdisk=max(allocated,peakdisk); st=fields(cg/'memory.stat');rc=[w.poll() for w in workers]; now=time.monotonic()-start
            rec.write(json.dumps(dict(sec=round(now,2),stage=last,allocated=allocated,proc_rw=counters,**{k:st[k] for k in ('file','anon','workingset_refault_file','pgmajfault')}))+'\n')
            if all(code is not None for code in rc):break
            over_reads |= read_guard > 0 and counters.get('producer',[0])[0] > read_guard
            if allocated>disk_limit or over_reads or any(code not in (0,None) for code in rc) or (stop_test and (r/'bands/band_0.lcp.done').exists()):
                if requested is None: requested=now
                (r/'stop.requested').touch(); md=r/'merged/modeb';
                if md.exists():(md/'stop.requested').touch()
            time.sleep(.5)
receipt=dict(order=a.order,overlap_binned=env.get('XSA_TEST_OVERLAP_BINNED','0'),overlap_pair_cap=env.get('XSA_OVERLAP_SORT_PAIRS'),overlap_stream=env.get('XSA_TEST_OVERLAP_STREAM','0'),cgroup_mib=a.mib,position_bounds=env.get('XSA_TEST_POSITION_BOUNDS','0'),position_merge=env.get('XSA_TEST_POSITION_MERGE','0'),integer_landing=env.get('XSA_TEST_INTEGER_LANDING','0'),read_guard=read_guard,over_reads=over_reads,emit_rows=env.get('XSA_TEST_EMIT_ROWS','dial_default'),wall_s=round(now,2),rc=rc,peak_disk=peakdisk,disk_guard=disk_limit,peak_memory=int((cg/'memory.peak').read_text()),events=fields(cg/'memory.events'),proc_attributed_rw=counters,refaults=st['workingset_refault_file'],major_faults=st['pgmajfault'])
receipt.update(prefix=env.get('XSA_OVERLAP_INLINE_PREFIX'),rounds=env.get('XSA_OVERLAP_RESTAGE'),carry=env.get('XSA_OVERLAP_CARRY_PRED'),phase_advice=env.get('XSA_OVERLAP_PHASE_ADVICE'),finish_stop_monitor='XSA_GATE_MONITOR_SO' in env,cancel_test=stop_test,stop_requested_s=requested)
if rc==[0,0]:
    with (r/'product.sxi2').open('rb') as f: receipt['sha256']=hashlib.file_digest(f,'sha256').hexdigest()
(r/'receipt.json').write_text(json.dumps(receipt,indent=2));print(receipt,flush=True)
if stop_test:
    assert requested is not None and now-requested<10 and rc==[75,75] and not over_reads and peakdisk<=disk_limit and not (r/'product.sxi2').exists();raise SystemExit(0)
assert not over_reads and peakdisk<=disk_limit and rc==[0,0] and receipt['sha256']=='6325c843c0d9aa9be00d69260aafdd794936e2e79f16174efb00c43db831f341'
