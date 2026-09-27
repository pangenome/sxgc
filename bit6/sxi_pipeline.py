#!/usr/bin/env python3
"""Build SXI + chi from endpoint-ready inputs; source mode fails preflight.

No legacy Phi index, aggregate oracle, or LF walk is used by the build mode.
Endpoint provenance is supplied by the caller; this is not a fresh front end.
"""
import argparse,json,pathlib,subprocess,sys,time,resource
ROOT=pathlib.Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',help='raw source or AGC path (currently blocked before launching any producer)')
p.add_argument('--ri4',help='normalized run table and mirrored tail samples')
p.add_argument('--heads',help='raw little-endian run-head SA samples')
p.add_argument('--parse',help='PFP parse/dictionary prefix')
p.add_argument('--anchors');p.add_argument('--names');p.add_argument('--output',required=True)
p.add_argument('--writer',default='/tmp/laneU/sxi_write');p.add_argument('--dump',default='/tmp/laneU/slim_dump')
p.add_argument('--xsa',default=str(ROOT/'xsa/target/release/xsa'));p.add_argument('--threads',type=int,default=8)
a=p.parse_args()
if a.source:
    sys.exit('BLOCKED: source -> endpoints has no validated tail-only Phi constructor, and the installed r-pfbwt emits heads rather than tails. See bit6/SXI_CONSTRUCTION_BLOCKER.md. No producer was launched.')
if not all((a.ri4,a.heads,a.parse)):p.error('need --ri4 --heads --parse (or --source to check fresh-build availability)')
if not 1<=a.threads<=64:p.error('--threads must be 1..64')
out=pathlib.Path(a.output).resolve();stage=pathlib.Path(str(out)+'.stage');agg=pathlib.Path(str(out)+'.agg');chi=pathlib.Path(str(out)+'.sA');log=pathlib.Path(str(out)+'.pipeline.jsonl')
for q in [out,stage,agg,chi,log]:
    if q.exists():sys.exit(f'refusing to overwrite {q}')
out.parent.mkdir(parents=True,exist_ok=True)
# Cap this process tree's address space; never signal unrelated processes.
# This conservative cap can reject a mmap-heavy stage before its RSS reaches 150 GB.
resource.setrlimit(resource.RLIMIT_AS,(149_000_000_000,149_000_000_000))
def run(cmd):
    begin=time.monotonic()
    with log.open('a')as f:
        f.write(json.dumps({'command':list(map(str,cmd)),'start':time.time()})+'\n');f.flush()
        result=subprocess.run(list(map(str,cmd)))
        f.write(json.dumps({'returncode':result.returncode,'wall_seconds':time.monotonic()-begin,'child_peak_kib':resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss})+'\n')
    if result.returncode:sys.exit(result.returncode)
writer=[a.writer,'--ri4',a.ri4,'--heads',a.heads]
for flag,value in [('--anchors',a.anchors),('--names',a.names)]:
    if value:writer += [flag,value]
run(writer+['--output',stage])
run([a.dump,'--slim','--resolve-ri4','--dict-stream','--sxi',stage,'--parse',a.parse,'-t',a.threads,'-o',agg])
run([a.xsa,'chi-rspace','--stream-agg','--sxi',stage,'--agg',agg,'-o',chi])
run(writer+['--chi',chi,'--output',out])
run([a.xsa,'sxi-info',out])
print(json.dumps({'sxi':str(out),'chi':str(chi),'provenance':'caller-supplied endpoint inputs; NOT a from-source gate'}))
