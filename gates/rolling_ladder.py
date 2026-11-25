"""Shared controls for sequential real v10 builds. All data scratch goes in a
capacity-limited private ext2. Never add image and guest allocation together.
"""
import fcntl, os, re, shutil, signal, stat, subprocess as sp, threading, time
from pathlib import Path
from overlap_real_v10_plan import FRONT,RANK,EMIT,TAP,FIN,MON,JOIN,FUSE,FSCK,MKFS,UMOUNT,PINS,need,sha
from overlap_real_v10_meter import grant,collect,load,save
from overlap_real_v10_map import S,MAP,TOTAL,TOTAL_DOCS,MANIFEST,SEPS,table
from overlap_target_prefix_certificate import SMALL,BIG

G=Path(__file__).resolve().parent; BASE=S.parent.parent
# raw bytes, input SHA, occurrences, shared GiB, producer array GiB
PROFILE={'1g':(1_000_000_665,'c382daa53d0e66fdc921c32d356961b7e8189ceee0a435bb3b66e06b203ebb61',31_784_026,8,3),
         '10g':(10**10,SMALL,318_164_488,48,22),'100g':(10**11,BIG,3_181_334_488,220,156)}
def toolpins(ps):
    for p in ps:need(p==p.resolve() and sha(p)==PINS[p],'tool pin: '+str(p))
def promotion(r):
    if r=='1g':return
    prev={'10g':'1g','100g':'10g'}[r];p=G/'receipts'/('rolling-ladder-'+prev)/'complete.json';need(p.is_file() and load(p).get('promote_next') is True,'prior rung not released: '+prev)
def statid(p):
    s=p.stat();return [s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns]
def syncdir(p):
    fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)

class Build:
    def __init__(self,r,d,end,phase):
        self.r=r;self.d=d;self.end=end;self.phase=phase;self.n,self.rawsha,self.m,gb,arr=PROFILE[r];self.ram=gb<<30;self.array=arr<<30
        need(d.parent==S and d.resolve()==d and re.fullmatch('rolling-build-'+r+'-[A-Za-z0-9]{6}',d.name) and d.stat().st_uid==os.getuid() and stat.S_IMODE(d.stat().st_mode)==0o700,'private ladder directory');promotion(r);self.before=grant(self.ram,4)
        self.q=d/'quota';self.img=d/'scratch.ext2';self.stop=d/'stop.requested';self.reserve=max(32<<20,self.n//100);self.image=(2*self.n-self.reserve)//4096*4096
        self.work={};self.jobs={};self.threads={};self.errors=[];self.mount_id=None;self.open_volume=False;self.peaks={};self.maxlog=0;self.latest=0;self.start=time.monotonic();self.step=phase;self.mhi=0;self.mstat={}
    def guard(self):
        need(time.time()<self.end and not self.stop.exists() and not self.errors,'STOP/deadline: '+str(self.errors));need(shutil.disk_usage(self.d).free>2*self.n,'insufficient volume free space')
    def launch(self,name,argv,src=None,pipe=False,env=None,kib=256):
        self.guard();need(name not in self.work,'duplicate job');args=list(map(str,argv));t=time.monotonic();e=dict(PATH='/usr/bin:/bin',HOME=str(Path.home()),LC_ALL='C',OMP_THREAD_LIMIT='4',OMP_NUM_THREADS='4',OMP_DYNAMIC='FALSE',OMP_STACKSIZE='2M',OMP_WAIT_POLICY='PASSIVE',GOMP_SPINCOUNT='0',RAYON_NUM_THREADS='2',MALLOC_ARENA_MAX='2')|(env or {})
        log=self.d/(self.phase+'-'+name+'.log');f=log.open('xb',buffering=0);cap=kib<<10;self.maxlog+=cap;need(self.maxlog<=self.reserve//2,'log reservation');os.posix_fallocate(f.fileno(),0,cap)
        p=sp.Popen(args,cwd=self.d,env=e,stdin=src or sp.DEVNULL,stdout=sp.PIPE,stderr=sp.PIPE if pipe else sp.STDOUT,start_new_session=True,bufsize=0);self.work[name]=p;self.jobs[name]=dict(argv=args,start=t)
        if src:src.close()
        fds=[f for f in (p.stdout,p.stderr) if f];need(all(0<fcntl.fcntl(fd,fcntl.F_GETPIPE_SZ)<=512<<10 for fd in fds),'pipe size')
        def drain():
            try:
                with f,(p.stderr if pipe else p.stdout) as stream:
                    siz=0
                    try:
                        while data:=stream.read(65536):
                            need(siz+len(data)<=cap,'log limit: '+name);f.write(data);siz+=len(data)
                    finally:f.truncate(siz)
            except Exception as ex:self.errors.append(str(ex));self.stop.touch()
        th=threading.Thread(target=drain,daemon=True);self.threads[name]=th;th.start();return p.stdout if pipe else None
    def inspect(self,k):
        p=self.work[k]
        if p.returncode is None:
            v=collect(p)
            if v is not None:self.jobs[k].update(v,wall_s=time.monotonic()-self.jobs[k]['start'])
    def sample(self):
        # Charge every logical scratch extent, including held-unlinked inodes.
        # Host image allocation is the physical ceiling; nested guest metadata,
        # including unlinked blocks, is already backed by that same image.
        seen={};dirs=0;aliases=unlinks=0
        def add(path,s):
            nonlocal aliases
            if not stat.S_ISREG(s.st_mode):return
            rel=Path(path).relative_to(self.d);a=rel.parts[0];kind='inputs' if a=='in' else 'guest' if a=='quota' else 'image' if a=='scratch.ext2' else 'output' if str(rel) in ('work.sxi2','work.sxi2.partial','index.sxi2') else 'controls'
            key=s.st_dev,s.st_ino
            if key in seen:need(seen[key][0]==kind,'cross-role alias');aliases+=1
            seen[key]=(kind,s.st_size,s.st_blocks*512)
        for dp,_,files in os.walk(self.d):
            path=Path(dp)
            if path!=self.q and self.q not in path.parents:dirs+=path.stat().st_blocks*512
            for a in files:
                p=path/a
                try:add(p,p.lstat())
                except FileNotFoundError:pass
        for k,p in self.work.items():
            if p.returncode is not None:continue
            try:
                for fd in Path(f'/proc/{p.pid}/fd').iterdir():
                    try:
                        name=os.readlink(fd)
                        if name.startswith(str(self.d)+'/') and name.endswith(' (deleted)'):add(name[:-10],fd.stat());unlinks+=1
                    except FileNotFoundError:pass
                with open(f'/proc/{p.pid}/maps') as f:need(not any(' (deleted)' in s and str(self.d)+'/' in s for s in f),'unsupported deleted mapping')
            except (FileNotFoundError,ProcessLookupError,PermissionError):
                self.inspect(k);need(p.returncode is not None,'could not account worker FDs')
        bins={k:[0,0] for k in ('inputs','guest','image','output','controls')}
        for k,n,b in seen.values():bins[k][0]+=n;bins[k][1]+=b
        bins['controls'][1]+=dirs;charge=bins['controls'][1]+bins['image'][1]+(8<<20);logical=bins['guest'][0]+bins['controls'][0]+(8<<20)
        need(charge<=2*self.n and logical<=2*self.n and bins['controls'][1]+(8<<20)<=self.reserve,'2N scratch refusal: '+str(bins))
        vals=dict(scratch_allocated_with_8MiB_transport_stage=charge,scratch_logical_with_8MiB_transport_stage=logical,open_unlinked=unlinks,aliases=aliases)
        vals.update({k+'_'+v:z[i] for k,z in bins.items() for i,v in enumerate(('logical','allocated'))});self.peaks.update({k:max(a,self.peaks.get(k,0)) for k,a in vals.items()})
        from overlap_real_v10_meter import cg
        c=cg();m=int((c/'memory.current').read_text());
        if m>self.mhi:
            self.mhi=m;stats=dict(line.split() for line in (c/'memory.stat').read_text().splitlines());self.mstat={k:int(stats[k]) for k in ('anon','file','shmem','slab','pagetables') if k in stats}
    def tick(self):
        self.guard()
        for k,p in self.work.items():self.inspect(k);need(p.returncode in (None,0),'job failed: '+k+' '+str(p.returncode))
        if self.open_volume:need(os.path.ismount(self.q) and self.work['fs'].returncode is None,'private mount lost')
        if time.monotonic()>self.latest:
            self.sample();self.latest=time.monotonic()+(2 if self.phase=='prepare' else 20 if self.r=='1g' else 120)
    def wait(self,k):
        while self.work[k].returncode is None:self.tick();time.sleep(.08)
        self.threads[k].join(3);need(not self.threads[k].is_alive() and self.work[k].returncode==0,'job/pipe exit: '+k);self.guard()
    def run(self,k,args,**opts):self.step=k;self.launch(k,args,**opts);self.wait(k);self.sample()
    def mountinfo(self):return [l.split() for l in Path('/proc/self/mountinfo').read_text().splitlines() if l.split()[4]==str(self.q)]
    def volume(self,create):
        if create:
            self.q.mkdir(mode=0o700)
            with self.img.open('xb') as f:os.posix_fallocate(f.fileno(),0,self.image);os.fsync(f.fileno())
        need(self.img.resolve()==self.img and self.img.stat().st_nlink==1 and self.img.stat().st_size==self.img.stat().st_blocks*512==self.image and not self.mountinfo() and not list(self.q.iterdir()),'backing identity/extent')
        if create:
            inodes=4096+4*((self.n+65535)//65536+(self.n+262143)//262144);self.run('mkfs',[MKFS,'-q','-t','ext2','-F','-b',4096,'-I',256,'-N',inodes,'-m',2,'-O','none,filetype,sparse_super,large_file','-E',f'nodiscard,lazy_itable_init=0,root_owner={os.getuid()}:{os.getgid()}',self.img]);
        self.launch('fs',[FUSE,'-s','-f','-o','rw,noatime,fsname='+str(self.img),self.img,self.q]);until=time.monotonic()+10
        while not (m:=self.mountinfo()):self.tick();need(time.monotonic()<until and self.work['fs'].returncode is None,'mount refused');time.sleep(.03)
        need(len(m)==1 and m[0][m[0].index('-')+2]==str(self.img),'foreign mount');self.mount_id=m[0][0];self.open_volume=True;self.q.chmod(0o700)
    def close(self):
        self.sample();m=self.mountinfo();need(len(m)==1 and m[0][0]==self.mount_id and m[0][m[0].index('-')+2]==str(self.img),'foreign unmount refused');self.open_volume=False;self.run('unmount',[UMOUNT,'-u',self.q]);self.wait('fs');need(not self.mountinfo(),'mount still live');self.run('fsck',[FSCK,'-f','-n',self.img]);self.mount_id=None
    def abort(self):
        self.stop.touch()
        if self.open_volume:
            for p in [self.q/'merged/modeb/stop.requested',self.q/'bands/producer.interrupted']:
                try:
                    if p.parent.is_dir():p.touch()
                except OSError:pass
        for sig in (None,signal.SIGTERM,signal.SIGKILL):
            for k,p in self.work.items():
                if k!='fs' and sig and p.returncode is None:
                    try:os.killpg(p.pid,sig)
                    except ProcessLookupError:pass
            until=time.monotonic()+1
            while time.monotonic()<until:
                for k in self.work:self.inspect(k)
                if all(k=='fs' or p.returncode is not None for k,p in self.work.items()):break
                time.sleep(.04)
        m=self.mountinfo()
        if m and m[0][m[0].index('-')+2]==str(self.img):sp.run([UMOUNT,'-u',self.q],check=True,timeout=8)
        if 'fs' in self.work:
            until=time.monotonic()+3
            while self.work['fs'].returncode is None and time.monotonic()<until:self.inspect('fs');time.sleep(.03)
        for th in self.threads.values():th.join(1)
    def report(self):
        return dict(rung=self.r,phase=self.phase,step=self.step,before=self.before,after=grant(self.ram,4),peaks=self.peaks,memory_stat_at_sampled_high=self.mstat,jobs=self.jobs,service_s=time.monotonic()-self.start,errors=self.errors,stop=self.stop.exists(),utc=time.strftime('%FT%TZ',time.gmtime()))
