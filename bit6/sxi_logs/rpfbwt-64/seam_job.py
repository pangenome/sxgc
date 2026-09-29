"""VALIDATION-ONLY subprocess runner; never invokes the retained front end."""
import json, os, re, subprocess, time
from pathlib import Path
LOG=Path(__file__).resolve().parent
OUT=LOG/'seam-policy-work'
BIN=LOG/'seam-policy-tools'
OUT.mkdir(exist_ok=True)
LIMIT=850_000_000_000

def record(**event):
    event.update(time=time.time(),validation_only=True)
    with (LOG/'validation-resume.jsonl').open('a') as f:
        f.write(json.dumps(event)+'\n');f.flush();os.fsync(f.fileno())

def run(stage,cmd,expected=0):
    cmd=list(map(str,cmd)); log=LOG/(stage+'.log'); timing=LOG/(stage+'.time')
    start=time.monotonic()
    with log.open('xb') as f:
        proc=subprocess.Popen(['/usr/bin/time','-v','-o',str(timing),'/usr/bin/prlimit',f'--as={LIMIT}',*cmd],stdout=f,stderr=f,env=dict(os.environ,OMP_NUM_THREADS='48'),start_new_session=True)
        record(stage=stage,status='START',command=cmd,pid=proc.pid,address_space_bytes=LIMIT)
        code=proc.wait()
    data=timing.read_text(); rss=re.search(r'Maximum resident set size \(kbytes\): (\d+)',data)
    record(stage=stage,status='PASS' if code==expected else 'FAIL',returncode=code,expected_returncode=expected,wall_seconds=time.monotonic()-start,max_rss_kib=int(rss[1]) if rss else None)
    if code!=expected: raise RuntimeError(f'{stage}: exit {code}, expected {expected}')
    return log.read_text()

if __name__=='__main__':
    run('seam466-measure',[BIN/'rpfbwt_endpoints.instrumented','/tmp/rpfbwt-64-466/parse',OUT/'measure.ri4',OUT/'measure.head_sa','1e'],expected=2)
