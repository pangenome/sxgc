"""Parent-invoked 1/10 GB front only. No path/size override and no next rung."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess as sp
import sys
import time

from overlap_resident_gate import CG, ONE, ROOT, TEN, cg_info, finished, footprint, proc_info, save
from overlap_timing_idle import exclusive
from private_cold import cold

RECEIPTS = Path(__file__).resolve().parent/'receipts'
SANDBOX = ROOT/'sandbox'
RUNGS = {'1g': (ONE.n, ROOT/'climb/rung1/pile-slice1g.bin', 'packed-parse-ORX2dw'),
         '10g': (TEN.n, ROOT.parent/'pf-10gb.txt', 'packed-front-10g-YebiCY')}
NAMES = ('dictionary.packed', 'parse.u32')
ONE_RAW_SHA = 'c382daa53d0e66fdc921c32d356961b7e8189ceee0a435bb3b66e06b203ebb61'


def need(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def load(d, name):
    return json.loads((d/name).read_text())


def prerequisite():
    d = RECEIPTS/'packed-serial-10g'
    i = load(d, 'identity.json')
    need(i['verified'] is True and i['sha256'] == i['expected'] == TEN.sha and
         i['reference_table_identical'] is True and len(i['members']) == 9 and
         load(d, 'build.json')['profile'] == 'packed-10g-bitmap' and load(d, 'oracle.json')['rc'] == 0 and
         (d/'exits.log').read_text().strip() == 'build=0 oracle=0' and
         (d/'oracle.log').read_text().startswith('ORACLE rows=10000000001 independent_final_rank=10000000001 all_adjacent_sa_lcp_bwt_verified=1 '),
         'published exact 10 GB verdict required')


def baseline(rung):
    if rung == '10g':
        b = load(RECEIPTS/'packed-front-10g', 'front-end.json')
        return b['wall_s'], {k:b['process'][k] for k in ('read_bytes','write_bytes')}, b['columns']
    d = RECEIPTS/'packed-parse-1g'
    log = (d/'pack.log').read_text()
    return (float(re.search(r'wall_s=([\d.]+)', (d/'pack.time').read_text())[1]),
            {'read_bytes':int(re.search(r'reads=(\d+)',log)[1]), 'write_bytes':None},
            dict(zip(NAMES,map(int,re.search(r'dictionary_bytes=(\d+) parse_bytes=(\d+)',log).groups()))))


def stat_io(text):
    need(text is not None, 'io.stat unavailable; enable cgroup IO accounting')
    totals = dict(rbytes=0,wbytes=0)
    for line in text.splitlines():
        fields = dict(f.split('=') for f in line.split()[1:])
        for k in totals:
            totals[k] += int(fields[k])
    return totals


def memory_available():
    return int(re.search(r'^MemAvailable:\s+(\d+) kB',Path('/proc/meminfo').read_text(),re.M)[1])*1024


def identity(path):
    st=path.stat();return (st.st_dev,st.st_ino,st.st_size,st.st_mtime_ns,st.st_ctime_ns)


def private_dir(d, rung):
    need(rung in RUNGS and d.resolve() == d and d.parent == SANDBOX and
         re.fullmatch(r'segmented-'+rung+'-[a-zA-Z0-9]{6}',d.name) and
         d.is_dir() and d.stat().st_uid == os.getuid() and d.stat().st_mode & 0o077 == 0,
         'expected fresh private segmented run directory')


def reviewed_one(path, pin):
    d=Path(path); private_dir(d,'1g')
    front,decode=(d/'segmented.json').read_bytes(),(d/'decode.json').read_bytes()
    need(hashlib.sha256(front+decode).hexdigest()==pin,'reviewed receipt digest mismatch')
    f,v=json.loads(front),json.loads(decode)
    need(f['rung']=='1g' and f['n']==ONE.n and f['status']=='FRONT_COMPARED' and
         f['rc']==0 and 0<f['wall_ratio']<3 and f['mincore_resident_pages']==0 and
         sum(baseline('1g')[2].values())<=f['census']['peak_counted']<=2*ONE.n and
         set(f['equal_column_sha256'])==set(NAMES) and v['status']=='INTERMEDIATE_VERIFIED' and
         all(re.fullmatch('[a-f0-9]{64}',s) for s in f['equal_column_sha256'].values()) and
         v['rc']==0 and v['decode_verified'] is True and v['raw_sha256']==f['raw_sha256']==ONE_RAW_SHA and
         not (d/'stop.requested').exists(),
         'reviewed 1 GB qualified front and independent decode required')
    for cg,mem in ((f['cgroup_front_after'],192<<20),(v['cgroup_after'],3<<30)):
        need(cg['memory.max']==str(mem) and cg['memory.swap.max']=='0' and cg['cpu.max']=='100000 100000' and
             {'oom 0','oom_kill 0'}.issubset(cg['memory.events'].splitlines()),'prior 1 GB grant/OOM')
    need(0<f['process']['read_bytes']<=16*ONE.n and 0<=f['process']['write_bytes']<=16*ONE.n and
         0<f['cgroup_front_io_delta']['rbytes']<=16*ONE.n and 0<=f['cgroup_front_io_delta']['wbytes']<=16*ONE.n,
         'prior 1 GB traffic guard')


def worker(rung, d, exe, deadline, report, prior):
    n, source, bank_name = RUNGS[rung]
    bank = SANDBOX/bank_name/'columns'
    raw = d/'in/corpus'; cols = d/'columns'; stop = d/'stop.requested'
    def guard():
        if time.time() >= deadline:
            stop.touch(exist_ok=True)
        need(not stop.exists(), 'deadline or cooperative stop')
    guard(); prerequisite(); exclusive()
    if rung=='10g':
        reviewed_one(*prior);report.update(reviewed_one_dir=prior[0],reviewed_one_sha256=prior[1])
    else:need(not prior,'unexpected prior receipt')
    need(set(x.name for x in d.iterdir())=={'front','verify','binaries.log'} and exe==d/'front', 'not a fresh gate build')
    cg = cg_info(); need((cg['cpu.max'],cg['memory.max'],cg['memory.swap.max']) ==
                         ('100000 100000',str(192<<20),'0'), 'incorrect grant')
    need((CG/'cgroup.procs').read_text().split() == [str(os.getpid())], 'not a dedicated run cgroup')
    stat_io(cg['io.stat']); base_s,base_io,sizes = baseline(rung)
    need(source.is_file() and source.stat().st_size == n and all((bank/k).is_file() and
         (bank/k).stat().st_size == sizes[k] for k in NAMES), 'missing/incorrect bank or source')
    source_identity=identity(source);bank_identity={k:identity(bank/k) for k in NAMES}
    free = shutil.disk_usage(d).free
    need(free >= 3*n+(1<<30), 'insufficient free space')
    report.update(rung=rung, n=n, source_identity=source_identity,bank_identity=bank_identity,immutable_source=str(source), trusted_columns=str(bank),
                  front_io_limit_each=16*n, deadline=deadline, free_before=free, reserved_logical=3*n, cgroup=str(CG), cgroup_before=cg)
    raw.parent.mkdir(); h=hashlib.sha256()
    with source.open('rb') as src, raw.open('xb') as out:
        left=n
        while left:
            guard(); block=src.read(min(left,1<<20));need(block,'short source');out.write(block);h.update(block);left-=len(block)
        need(not src.read(1),'source grew')
    need(identity(source)==source_identity,'source changed during copy')
    raw.chmod(0o400);report.update(private_input_bytes=n,raw_sha256=h.hexdigest())
    if rung=='1g':need(h.hexdigest()==ONE_RAW_SHA,'rung1 hash')
    guard(); cold(raw);report.update(cold_private_path=str(raw), mincore_resident_pages=0,
                                        mincore_checked_pages=(n+os.sysconf('SC_PAGE_SIZE')-1)//os.sysconf('SC_PAGE_SIZE'))
    save(d/'preflight.json',report)
    guard(); before=cg_info(); bi=stat_io(before['io.stat'])
    need(bi['wbytes']>0,'private copy did not register cgroup IO')
    t=time.monotonic(); peak=logical=rss=whole=0; min_free=free; reason=None
    with (d/'front.log').open('xb') as log, sp.Popen([exe,raw,cols,'--intermediate-'+rung],stdin=sp.PIPE,stdout=log,stderr=log) as p:
        try:
            p.stdin.write(f'SEGMENTED_PREFLIGHT {os.getpid()} {n} {deadline}\n'.encode());p.stdin.close()
            with open(f'/proc/{p.pid}/io') as io:
                while True:
                    done=finished(p); info=proc_info(p.pid,io); rss=max(rss,info['VmHWM'])
                    ai=stat_io((CG/'io.stat').read_text());delta={k:ai[k]-bi[k] for k in ai}
                    alloc,parts=footprint(d,0);peak=max(peak,alloc)
                    column_bytes=parts.get('generated_lexparse',0);logical=max(logical,column_bytes)
                    whole=max(whole,n+column_bytes+sum(x.stat().st_size for x in d.iterdir() if x.is_file()))
                    min_free=min(min_free,shutil.disk_usage(d).free)
                    if time.time()>=deadline or stop.exists() or min_free<(256<<20) or logical>2*n or max(info['read_bytes'],info['write_bytes'],*delta.values())>16*n:
                        if not reason:reason='time/stop/IO/space guard';stop.touch(exist_ok=True);grace=time.monotonic()+5
                        if not done and time.monotonic()>=grace:p.kill()
                    if done:break
                    time.sleep(.25)
            wall=time.monotonic()-t;rc=p.wait()
        except BaseException:
            stop.touch(exist_ok=True);p.kill();raise
    after=cg_info(); bi=stat_io(before['io.stat']); ai=stat_io(after['io.stat']);delta={k:ai[k]-bi[k] for k in ai}
    report.update(front_wall_s=wall, rc=rc, process=info, sampled_rss=rss, cgroup_front_before=before,cgroup_front_after=after,
                  cgroup_front_io_delta=delta, sampled_counted_logical=logical, sampled_whole_dir_allocated=peak,
                  sampled_whole_dir_file_bytes=whole,
                  sampled_min_free=min_free,free_after_front=shutil.disk_usage(d).free)
    need(not reason and rc==0 and {'oom 0','oom_kill 0'}.issubset(after['memory.events'].splitlines()),'front stopped or OOM')
    need(info['read_bytes']>0 and delta['rbytes']>0 and min(delta.values())>=0 and max(delta.values())<=16*n,'missing physical IO evidence or cgroup IO guard')
    census=dict((k,int(v)) for k,v in re.findall(r'(\w+)=(\d+)',(d/'front.log').read_text().split('LEX100 ',1)[1]))
    need(census['n']==n and sum(sizes.values())<=census['peak_counted']<=2*n and {x.name:x.stat().st_size for x in cols.iterdir()}==sizes,'incomplete front')
    report.update(census=census,logical_headroom=2*n-census['peak_counted'],archived_front_wall_s=base_s,
                  archived_front_io=base_io,wall_ratio=wall/base_s,
                  attributed_io_ratio={k:info[k]/v if v else None for k,v in base_io.items()})
    # Independent byte comparisons; open the pinned bank only for sequential reads.
    hashes={}; start=time.monotonic()
    for name in NAMES:
        digest=hashlib.sha256()
        with (cols/name).open('rb') as new,(bank/name).open('rb') as trusted:
            left=sizes[name]
            while left:
                guard(); block=new.read(min(left,1<<20));need(block and trusted.read(len(block))==block,'bank mismatch: '+name)
                digest.update(block);left-=len(block)
            need(not new.read(1) and not trusted.read(1),'comparison extent')
        hashes[name]=digest.hexdigest()
    need(bank_identity=={k:identity(bank/k) for k in NAMES},'bank identity/metadata changed')
    guard();report.update(equal_column_sha256=hashes,compare_s=time.monotonic()-start, target_under_2x=wall<2*base_s)
    report['front_under_3x']=wall<3*base_s


if __name__=='__main__':
    rung,d,exe,deadline,*prior=sys.argv[1:];d=Path(d); report=dict(status='REFUSED',promotion=False)
    private_dir(d,rung)
    need(not (d/'segmented.json').exists(), 'refuse receipt reuse')
    try:
        worker(rung,d,Path(exe),int(deadline),report,prior);report['status']='FRONT_COMPARED'
    except Exception as e:
        report['error']=str(e);raise
    finally:
        save(d/'segmented.json',report)
