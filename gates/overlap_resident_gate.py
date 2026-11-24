"""Measured, rung1-only, from prepared lexparse. Invoked by overlap_resident_gate.sh.
Build stdout is reserved for the separate raw verifier. No Python per-row path.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess as sp
import sys
import time

REPO = Path(__file__).resolve().parents[1]
BIN = REPO/'xsa/target/release'
N = 1_000_000_665
SHA = 'b20ceebe91707dbac80c2ea12c99e704c6b6a734293f3a558c90ce090c21f6a3'
ANCHOR = Path('/mnt/nvme2n1/erikg/TAPEREADER/climb/rung1/run_wide/product.sxi2')
GIB = 1 << 30
CG = Path('/sys/fs/cgroup')/Path('/proc/self/cgroup').read_text().strip().split('::/')[1]

def save(p, obj): p.write_text(json.dumps(obj, indent=2, sort_keys=True)+'\n')
def say(*args): print(*args, file=sys.stderr, flush=True)
def sha(p):
    with p.open('rb') as f: return hashlib.file_digest(f,'sha256').hexdigest()
def members(p):
    with p.open('rb') as f:
        header=f.read(64); assert header[:8]==b'SXI2\x08\0\0\0' and struct.unpack_from('<I',header,32)[0]==9
        end=f.seek(-9*40,2); tab={}
        for _ in range(9):
            mid,codec,off,size,ct=struct.unpack_from('<IIQQQ',f.read(40))
            assert mid not in tab and 64<=off<=off+size<=end; tab[mid]=(codec,off,size,ct)
    assert set(tab)=={1,4,5,7,10,11,12,13,14}; return tab

def cg_info():
    data={x:(CG/x).read_text().strip() for x in ('memory.max','memory.swap.max','memory.peak','memory.events','cpu.stat','cpu.max')}
    data['io.stat']=(CG/'io.stat').read_text().strip() if (CG/'io.stat').exists() else None
    return data

def proc_info(pid,iofile):
    p=Path('/proc')/str(pid); status=(p/'status').read_text()
    # Reopening /proc/<zombie>/io may be denied; an FD opened while live
    # still yields final counters before wait(). Never substitute zeros.
    iofile.seek(0); data={k:int(v) for k,v in (l.split(': ') for l in iofile.read().splitlines())}
    for k in ('VmHWM','VmRSS','RssAnon'):
        m=re.search(r'^'+k+r':\s+(\d+) kB',status,re.M); data[k]=int(m[1])*1024 if m else 0
    # comm can contain spaces. Fields 14/15 are user/system clock ticks.
    fields=(p/'stat').read_text().rsplit(')',1)[1].split(); hz=os.sysconf('SC_CLK_TCK')
    data['cpu_s']=(int(fields[11])+int(fields[12]))/hz
    return data

def finished(p):
    # Read final /proc/io and CPU counters *before* wait reaps the zombie.
    return os.waitid(os.P_PID,p.pid,os.WEXITED|os.WNOHANG|os.WNOWAIT) is not None

def footprint(d,retained):
    # Logical sizes count even if sparse. Sample allocated bytes separately,
    # including private copies, logs and the partial/final product.
    whole=0; parts={'prepared_refs_ids':retained}; stack=[d]
    while stack:
        try: entries=os.scandir(stack.pop())
        except FileNotFoundError: continue  # finish retired the pieces directory
        for e in entries:
            try: st=e.stat(follow_symlinks=False)
            except FileNotFoundError: continue
            whole+=st.st_blocks*512
            if e.is_dir(follow_symlinks=False):stack.append(e.path);continue
            rel=Path(e.path).relative_to(d); f=e.name
            if rel.parts[0]=='in' or len(rel.parts)==1 and (f in ('oracle-corpus','inverse.check','product.sxi2','product.sxi2.partial','stop.requested') or f.endswith(('.log','.json','.sha'))):continue
            group=rel.parts[0] if len(rel.parts)==1 else ('pieces' if '.pieces' in str(rel.parent) else 'slices' if rel.parts[0]=='bands' else f)
            parts[group]=parts.get(group,0)+st.st_size
    return whole,parts

def oracle(d,tools):
    assert (CG/'memory.max').read_text().strip()==str(1400<<20) and (CG/'memory.swap.max').read_text().strip()=='0'
    start=time.monotonic(); peak=0
    with (d/'oracle.log').open('xb') as f:
        p=sp.Popen([tools/'oracle',d/'oracle-corpus'],stdout=f,stderr=f)
        with open(f'/proc/{p.pid}/io') as io:
            while not finished(p): peak=max(peak,proc_info(p.pid,io)['VmHWM']); time.sleep(.5)
            stats=proc_info(p.pid,io); rc=p.wait()
    save(d/'oracle.json',dict(rc=rc,process=stats,sampled_peak_rss=peak,cgroup=cg_info(),wall_including_wait_s=time.monotonic()-start))
    assert rc==0 and f'ORACLE rows={N+1} independent_final_rank={N+1} all_adjacent_sa_lcp_bwt_verified=1' in (d/'oracle.log').read_text()

def build(d,tools):
    assert (CG/'memory.max').read_text().strip()==str(20*GIB) and (CG/'memory.swap.max').read_text().strip()=='0'
    src=d/'in'; corpus=src/'corpus'; retained=sum((src/f).stat().st_size for f in ('parse.u32','dictionary.refs')); assert corpus.stat().st_size==N
    refs=members(ANCHOR); rows=N+1
    # Same finish profile; reserve its full-sized named spools conservatively
    # even though some do not coexist. 64 slices (all escapes worst case).
    # 32 outstanding pieces + 64-release batch, plus 8 for reader/frontier.
    per_piece=44+5*65536+16*((rows.bit_length()*65536+63)//64)
    plan=dict(retained=retained,pfck=64+8*(rows//22+2),bitmap=(rows+7)//8,sparse=refs[11][2],
              lcp_runs=refs[14][2],ef_spools=refs[5][2],pieces=104*(per_piece+40),
              slices=64*(48+17*262144),control_margin=8<<20)
    bound=sum(plan.values()); assert bound<2*N
    save(d/'scratch-plan.json',dict(parts=plan,counted_bound=bound,limit=2*N))
    (d/'bands').mkdir(); (d/'merged').mkdir(); flag=d/'stop.requested'; start=time.monotonic()
    cg0=cg_info(); env={k:v for k,v in os.environ.items() if not k.startswith(('XSA_','SS_','CROSS_','PFCK_','TRIE_','LD_PRELOAD'))}
    env.update(SS_LCP_SLICE_WINDOW='64',CROSS_V2_PIECES='1',CROSS_V2_PIECE_WINDOW='32',PFCK_SPACING='22')
    common=['--corpus',corpus,'--remap',src/'remap','--threads','2']; pref=d/'merged/frag'
    w={}; proc={}; iofiles={}; exitcode={}; times={}; peaks={}; logs={}; started={}; tail=None
    def launch(name,cmd,**kwargs):
        logs[name]=(d/(name+'.log')).open('xb'); started[name]=time.monotonic()
        kwargs.setdefault('stdout',logs[name]); w[name]=sp.Popen(cmd,stderr=logs[name],**kwargs); peaks[name]=0
        iofiles[name]=open(f'/proc/{w[name].pid}/io')
    def request(reason):
        if not flag.exists():say('STOP_REQUEST',reason); flag.write_text(reason+'\n')
        md=d/'merged/modeb';
        if md.exists():(md/'stop.requested').touch(exist_ok=True)
    audit=os.dup(1)
    launch('resident',[BIN/'overlap_resident',src,flag,str(14*GIB),d/'inverse.check'],
           env=dict(env,XSA_RESIDENT_AUDIT_FD=str(audit)),stdout=sp.PIPE,pass_fds=(audit,)); os.close(audit)
    counted_peak=0; disk_peak=0; at_peak={}; each={}; last_disk=0; active='start'; samples={}
    with (d/'resident.log').open() as tail:
        while True:
            for l in tail.readlines():
                if l.startswith('RESIDENT_'):say(l.rstrip())
                if l.startswith('RESIDENT_BEGIN'):active=l.strip().split('=')[-1]
            if active=='emit' and 'sink' not in w and not flag.exists():
                # Don't charge a sort's idle wall to the finish. Frozen data path.
                launch('sink',[BIN/'pfp_sort_r',*common,'--out-prefix',pref,'--bands',d/'bands','--scratch-budget-bytes',str(2*N-retained)],
                       env=dict(env,XSA_TEST_RESIDENT_TRIPLES='1'),stdin=w['resident'].stdout,stdout=sp.DEVNULL); w['resident'].stdout.close()
                launch('finish',['/tmp/xsa_finish_cur',*common,'--prefix',pref,'--lcp-bands',d/'bands','--output',d/'product.sxi2','--poll-timeout','0','--wit-ram-budget',str(2*GIB),'--with-chi','--container-stream'],
                       env=dict(env,LD_PRELOAD=str(tools/'monitor.so'),XSA_GATE_PRIVATE_STOP=str(flag)))
            for name,p in w.items():
                if name in exitcode: continue
                done=finished(p); info=proc_info(p.pid,iofiles[name]);proc[name]=info;peaks[name]=max(peaks[name],info['VmHWM'])
                if done:
                    exitcode[name]=p.wait();times[name]=time.monotonic()-started[name];say('EXIT',name,exitcode[name],round(times[name],3))
                    if exitcode[name]: request(f'{name} rc={exitcode[name]}')
            if time.monotonic()-last_disk>1 or len(exitcode)==len(w):
                last_disk=time.monotonic(); whole,parts=footprint(d,retained); disk_peak=max(disk_peak,whole)
                counted=sum(parts.values());
                for k,v in parts.items(): each[k]=max(each.get(k,0),v)
                if counted>counted_peak: counted_peak=counted;at_peak=parts
                if counted>bound:request('counted scratch crossed reservation')
            mem=int((CG/'memory.current').read_text()); samples[active]=max(samples.get(active,0),mem)
            if flag.exists():request('private flag')
            if sum(x['read_bytes'] for x in proc.values())>20*N or time.monotonic()-start>3600:request('runaway read/time guard')
            if len(exitcode)==len(w):break
            time.sleep(.2)
    for f in (*logs.values(),*iofiles.values()):f.close()
    rc=dict(rc=exitcode,sampled_wall_s=times,phase_sampled_shared=samples,process=proc,sampled_peak_rss=peaks,
            counted_sampled_peak=counted_peak,counted_at_peak=at_peak,individual_sampled_peaks=each,whole_dir_sampled_allocated=disk_peak,
            wall_s=time.monotonic()-start,shared_before=cg0,shared_after=cg_info(),stop=flag.exists())
    save(d/'build.json',rc)
    assert exitcode==dict(resident=0,sink=0,finish=0) and not flag.exists()
    assert 'oom_kill 0' in rc['shared_after']['memory.events']

def certify(d):
    actual=sha(d/'product.sxi2'); table=members(d/'product.sxi2'); same=table==members(ANCHOR)
    rank_ok=f'RESIDENT_CHECK inverse_ranks=31784026 virtual_end=0' in (d/'resident.log').read_text()
    good=actual==SHA and same and rank_ok and json.loads((d/'oracle.json').read_text())['rc']==0
    save(d/'identity.json',dict(verified=good,sha256=actual,expected=SHA,bytes=(d/'product.sxi2').stat().st_size,members=table,reference_table_identical=same))
    assert good; print('PASS resident ranks, all raw SA/LCP/BWT rows, exact nine-member SHA',actual,flush=True)

if __name__=='__main__':
    mode=sys.argv[1]; d=Path(sys.argv[2]);
    if mode=='certify':certify(d)
    elif mode=='oracle':oracle(d,Path(sys.argv[3]))
    elif mode=='build':build(d,Path(sys.argv[3]))
    else:raise ValueError(mode)
