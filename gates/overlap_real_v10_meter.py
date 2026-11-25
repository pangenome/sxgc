"""Fixed Pile product services: bounded logs, child wait4 and recursive inode accounting."""
import fcntl
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import stat
import subprocess as sp
import sys
import threading
import time

from overlap_real_v10_plan import CONTROL,MEM,N,ORACLE,PRODUCT,TRANSPORT,image_bytes,outside_reserve,need
from overlap_v10_product_compare import optional_io_stat

clock=time.monotonic

def save(p,obj):
    with p.open('x') as f:json.dump(obj,f,indent=2,sort_keys=True);f.write('\n');f.flush();os.fsync(f.fileno())
def load(p):return json.loads(p.read_text())
def cg():return Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().strip().split('::')[1])
def grant(ram,cpu):
    d=cg();read=lambda k:(d/k).read_text().strip();v={k:read(k) for k in ('memory.max','memory.peak','memory.swap.max','memory.events','cpu.max','cpu.stat')};v['io.stat']=optional_io_stat(d);v['cores']=sorted(os.sched_getaffinity(0));need((v['memory.max'],v['cpu.max'],v['memory.swap.max'])==(str(ram),f'{cpu}00000 100000','0') and len(v['cores'])==cpu and {'oom 0','oom_kill 0'}<=set(v['memory.events'].splitlines()),'cgroup or OOM gate');return v

def collect(p,block=False):
    pid,status,r=os.wait4(p.pid,0 if block else os.WNOHANG)
    if not pid:return None
    p.returncode=os.waitstatus_to_exitcode(status)
    return dict(rc=p.returncode,cpu_s=r.ru_utime+r.ru_stime,maxrss_bytes=1024*r.ru_maxrss,ru_inblock_bytes=512*r.ru_inblock,ru_oublock_bytes=512*r.ru_oublock,major_faults=r.ru_majflt,minor_faults=r.ru_minflt)

def files(root,children,inspect):
    # Product reservation is separate, INCLUDING the C++ writer's .partial.
    # Dedupe writer slot hard links and open-but-unlinked regular files.
    seen={};aliases=0;unlinked=0;host_dirs=0
    def add(name,st):
        nonlocal aliases
        if not stat.S_ISREG(st.st_mode):return
        parts=Path(name).relative_to(root).parts
        k='scratch'
        if len(parts)>=2 and parts[0] in ('reference','candidate'):
            if parts[1]=='quota':k='guest'
            if parts[1:]==('scratch.ext4',):k='image';need(st.st_size==image_bytes(parts[0]),'backing image extent drift')
            if parts[1:] in (('in','raw'),('in','checker'),('in','remap'),('in','map','seps.bin')):k='inputs'
            if parts[1:] in (('work.sxi2.partial',),('work.sxi2',),('index.sxi2',)):k='output';need(st.st_size<=PRODUCT,'product limit')
        key=st.st_dev,st.st_ino;old=seen.get(key)
        if old:aliases+=1;need(old[0]==k,'cross-category file alias')
        seen[key]=(k,st.st_size,st.st_blocks*512)
    for path,_,names in os.walk(root):
        parts=Path(path).relative_to(root).parts
        if len(parts)<2 or parts[1]!='quota':
            try:host_dirs+=Path(path).stat().st_blocks*512
            except FileNotFoundError:pass
        for name in names:
            f=Path(path,name)
            try:add(f,f.lstat())
            except FileNotFoundError:pass
    for name,p in children.items():
        if p.returncode is not None:continue
        for attempt in range(12):
            try:
                for fd in Path(f'/proc/{p.pid}/fd').iterdir():
                    try:
                        path=os.readlink(fd)
                        if path.startswith(str(root)+'/'):
                            deleted=path.endswith(' (deleted)');unlinked+=int(deleted);add(path.removesuffix(' (deleted)'),fd.stat())
                    except FileNotFoundError:pass
                # A deleted solely-mapped file cannot get a false release credit.
                with open(f'/proc/{p.pid}/maps') as f:
                    for line in f:
                        if ' (deleted)' in line and str(root)+'/' in line:raise RuntimeError('private deleted mapping still live: '+line.rstrip())
                break
            except (FileNotFoundError,ProcessLookupError,PermissionError):
                inspect(name,p)
                if p.returncode is not None:break
                if attempt==11:raise
                time.sleep(.01)
    sums={k:[0,0] for k in ('scratch','guest','image','inputs','output')}
    for k,n,b in seen.values():sums[k][0]+=n;sums[k][1]+=b
    outside=sums.pop('scratch');sums['outside']=[outside[0],outside[1]+host_dirs]
    # Two views of the same physical blocks. Count guest logical extents and
    # host-control extents; count the whole backing allocation ONCE for disk.
    sums['scratch']=[outside[0]+sums['guest'][0],sums['outside'][1]+sums['image'][1]]
    return sums,unlinked,aliases

class Group:
    def __init__(self,root,name,end):
        self.root=root;self.name=name;self.d=root/name;self.stop=self.d/'stop.requested';self.end=end;self.work={};self.starts={};self.results={};self.argv={};self.errors=[];self.drains=[];self.log_total=0;self.last=0;self.memhi=0;self.memparts={};self.peaks={};self.measure_at={};self.abi={};self.until_reason=None;self.boot=clock();self.bound=2*N;self.timing={};self.phase='setup';self.before=grant(MEM,4);self.daemons=set();self.volume=None
    def guard(self):
        if self.volume and self.volume.lease is not None:need(self.volume.daemon.returncode is None and os.path.ismount(self.volume.d),'live quota filesystem lost')
        need(time.time()<self.end and not self.stop.exists() and not (self.root/'stop.requested').exists() and not self.errors,'STOP/deadline/log cap: '+str(self.errors));need(shutil.disk_usage(self.root).free>512<<20,'disk headroom')
    def drain(self,pipe,path,cap):
        try:
            with pipe,path.open('xb',buffering=0) as f:
                # Pre-size a bounded control inode instead of XFS growing an
                # append at EOF. At close retain exactly the observed bytes.
                os.posix_fallocate(f.fileno(),0,cap);n=0
                while data:=pipe.read(8192):
                    n+=len(data);self.log_total+=len(data)
                    need(n<=cap and self.log_total<=1536<<10,'log limit: '+path.name);f.write(data)
                f.truncate(n)
        except Exception as e:self.errors.append(str(e));self.stop.touch()
    def launch(self,name,args,env=None,src=None,stream=False,data=None,fsize=PRODUCT):
        self.guard();need(name not in self.work,'duplicate worker');base=dict(PATH='/usr/bin:/bin',HOME=str(Path.home()),LANG='C',LC_ALL='C',MALLOC_ARENA_MAX='2',OMP_NUM_THREADS='2',OMP_THREAD_LIMIT='4',OMP_DYNAMIC='FALSE',OMP_STACKSIZE='2M',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0',RAYON_NUM_THREADS='2')
        lim=['/usr/bin/prlimit',f'--fsize={fsize}:{fsize}','--stack=8388608:8388608'];args=list(map(str,args))
        if data:lim.append(f'--data={data}:{data}')
        e=base|(env or {});cmd=lim+['--']+args;self.argv[name]=dict(cmd=cmd,env=e);self.starts[name]=clock();p=sp.Popen(cmd,env=e,cwd=self.d,start_new_session=True,stdin=src if src is not None else sp.DEVNULL,stdout=sp.PIPE,stderr=sp.PIPE if stream else sp.STDOUT,bufsize=0);self.work[name]=p
        if src is not None:src.close()
        sizes=[fcntl.fcntl(fd,fcntl.F_GETPIPE_SZ) for fd in (p.stdout,p.stderr) if fd is not None];self.abi[name]=sizes;need(all(0<x<=65536 for x in sizes) and sum(sum(self.abi[k]) for k,v in self.work.items() if v.returncode is None)<=TRANSPORT,'pipe reservation')
        # 382 slice diagnostics on the real sink. Keep the aggregate log guard;
        # the first reference correctly STOPped on its too-small 64 KiB subcap.
        cap=(512 if name in ('finish','tap') else 192 if name=='sink' else 64)<<10;th=threading.Thread(target=self.drain,args=(p.stderr if stream else p.stdout,self.d/(name+'.log'),cap),daemon=True);th.start();self.drains.append((name,th))
        return p.stdout if stream else None
    def inspect(self,name,p):
        if p.returncode is None:
            r=collect(p)
            if r is not None:self.results[name]=r|{'wall_s':clock()-self.starts[name]}
    def sample(self):
        sums,u,a=files(self.root,self.work,self.inspect)
        if self.volume and self.volume.sealed_bytes and self.volume.img.exists() and not self.volume.mountinfo():sums['scratch'][0]+=self.volume.sealed_bytes
        vals={k+'_'+label:s[i] for k,s in sums.items() for i,label in enumerate(('bytes','allocated'))};vals['whole_allocated']=sum(sums[k][1] for k in ('scratch','inputs','output'));vals['outer_charged']=sums['scratch'][1]+outside_reserve(self.name)-CONTROL;vals.update(open_unlinked_fds=u,aliases=a)
        for k,v in vals.items():
            if v>self.peaks.get(k,-1):self.peaks[k]=v;self.measure_at[k]=self.phase
        need(sums['outside'][1]<=CONTROL and sums['image'][1]<=image_bytes(self.name) and vals['outer_charged']<=2*N,'hardware allocation/control/transport cap: '+str(vals));need(vals['scratch_bytes']<=self.bound,'sampled scratch vs phase reservation: '+str(vals));d=cg();current=int((d/'memory.current').read_text());m={k:int(v) for k,v in (l.split() for l in (d/'memory.stat').read_text().splitlines())}
        if current>self.memhi:self.memhi=current;self.memparts={k:m[k] for k in ('anon','file','shmem','sock','kernel_stack','pagetables','slab') if k in m}
        # Last closed workers have wait4 IO (in kernel 512-byte block units).
        # Bound actual attributed traffic; pipe traffic is not disk traffic.
        reads=sum(r['ru_inblock_bytes'] for r in self.results.values());writes=sum(r['ru_oublock_bytes'] for r in self.results.values())
        for name,p in self.work.items():
            if p.returncode is not None:continue
            try:
                vals=dict(l.split(': ') for l in Path(f'/proc/{p.pid}/io').read_text().splitlines());reads+=int(vals['read_bytes']);writes+=int(vals['write_bytes'])
            except (FileNotFoundError,ProcessLookupError,PermissionError):
                self.inspect(name,p) # exited tasks still have mandatory wait4 counters
        need(reads<=(6<<30) and writes<=(4<<30),'native attributed IO limit')
    def tick(self):
        self.guard()
        for name,p in self.work.items():self.inspect(name,p);need(p.returncode in (0,None),'worker failure: '+name+' '+str(p.returncode))
        if clock()-self.last>=.8:self.sample();self.last=clock()
    def until(self,test):
        while True:
            self.tick()
            if test():return
            time.sleep(.04)
    def wait(self):
        self.until(lambda:all(p.returncode is not None for k,p in self.work.items() if k not in self.daemons))
        for k,th in self.drains:
            if k not in self.daemons or self.work[k].returncode is not None:th.join(3);need(not th.is_alive(),'log did not close')
        self.guard();need(all(p.returncode==0 for k,p in self.work.items() if k not in self.daemons),'nonzero worker at join');self.sample()
    def run(self,name,args,**kw):self.launch(name,args,**kw);self.wait()
    def abort(self):
        self.stop.touch(exist_ok=True);(self.root/'stop.requested').touch(exist_ok=True)
        for q in (self.d/'quota/stop.requested',self.d/'quota/merged/modeb/stop.requested',self.d/'quota/bands/producer.interrupted',self.d/'quota/tap-bands/producer.interrupted'):
            if q.parent.is_dir():q.touch(exist_ok=True)
        for sig in (None,signal.SIGTERM,signal.SIGKILL):
            until=clock()+(3 if sig is None else 1)
            for k,p in self.work.items():
                if k in self.daemons:continue
                if sig and p.returncode is None:
                    try:os.killpg(p.pid,sig)
                    except ProcessLookupError:pass
            while clock()<until:
                for k,p in self.work.items():self.inspect(k,p)
                if all(p.returncode is not None for k,p in self.work.items() if k not in self.daemons):return
                time.sleep(.03)
    def report(self):
        try:after=grant(MEM,4)
        except (OSError,ValueError) as err:after={'resource_status_error':str(err),'memory.events':(cg()/'memory.events').read_text().strip()}
        own=resource.getrusage(resource.RUSAGE_SELF)
        return dict(supervisor_io_including_prep_and_proof=dict(ru_inblock_bytes=512*own.ru_inblock,ru_oublock_bytes=512*own.ru_oublock),worker=self.results,argv=self.argv,log_bytes=self.log_total,log_errors=self.errors,pipe_capacity=self.abi,clocks=self.timing,peaks=self.peaks,sampled_high_phase=self.measure_at,sampled_memory_high=self.memhi,memory_stat_at_sample=self.memparts,before=self.before,after=after,service_body_s=clock()-self.boot,stop=self.stop.exists(),utc=time.strftime('%FT%TZ',time.gmtime()))

# Independent unit inherits only the anonymous input/output pipes. No row file,
# no scratch reader, no mutable corpus, no sharing builder CPU or memory budget.
def verify(d,role,end,core):
    from overlap_real_v10_plan import CHECK
    before=grant(ORACLE,1);need(before['cores']==[core] and d==d.resolve() and d.name==role and role in ('candidate','reference') and time.time()<end and not (d/'stop.requested').exists(),'verifier authority');mode='rolling' if role=='candidate' else 'reference';cmd=list(map(str,[CHECK,'--real-v10-'+mode+'-100m',d/'in/checker',d/'stop.requested',end]));t=clock();p=sp.Popen(cmd);r=collect(p,True);need(r is not None,'lost native verifier');save(d/'checker-meter.json',dict(worker=r,wall_s=clock()-t,cmd=cmd,before=before,after=grant(ORACLE,1)));need(r['rc']==0 and r['ru_inblock_bytes']<=2*N and r['ru_oublock_bytes']<=4<<20,'witness IO/exit');return 0
if __name__=='__main__':sys.exit(verify(Path(sys.argv[1]),sys.argv[2],int(sys.argv[3]),int(sys.argv[4])))
