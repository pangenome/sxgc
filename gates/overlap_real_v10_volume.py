"""A preallocated private block device file bounds physical scratch, including FS metadata.
All FUSE CPU/cache belongs to the same build service. Never mount or unmount another job's volume.
"""
import os
from pathlib import Path
import signal
import subprocess as sp
import time
from overlap_real_v10_plan import FUSE,FSCK,MKFS,UMOUNT,CONTROL,N,image_bytes,outside_reserve,guest_need,need
from overlap_real_v10_meter import clock,save
from private_cold import cold

class Volume:
    def __init__(self,h,m20):
        self.h=h;self.d=h.d/'quota';self.img=h.d/'scratch.ext2';self.ext=image_bytes(h.name);self.require=guest_need(h.name,m20);self.num=0;self.lease=None;self.epochs=[];self.tag=None;self.inode=None;self.daemon=None;self.sealed_bytes=0
    def ident(self):s=self.img.lstat();return s.st_dev,s.st_ino,s.st_size,s.st_nlink
    def mountinfo(self):
        lines=[l.split() for l in Path('/proc/self/mountinfo').read_text().splitlines() if l.split()[4]==str(self.d)];need(len(lines)<=1,'stacked private mount');return lines[0] if lines else None
    def owned(self):
        m=self.mountinfo();need(m and m[m.index('-')+1].startswith('fuse') and m[m.index('-')+2]==str(self.img) and self.ident()==self.inode and (self.lease is None or m[0]==self.lease),'not the private owned mount');return m[0]
    def create(self):
        h=self.h;t=clock();self.d.mkdir(mode=0o700);need(self.ext+outside_reserve(h.name)<=2*N and not self.mountinfo(),'backing capacity');
        with self.img.open('xb') as f:os.posix_fallocate(f.fileno(),0,self.ext);os.fsync(f.fileno());s=os.fstat(f.fileno());need(s.st_blocks*512==s.st_size==self.ext,'backing allocation')
        self.inode=self.ident();h.run('mkfs',[MKFS,'-t','ext2','-q','-F','-b',4096,'-N',4096,'-I',256,'-m',2,'-O','none,filetype,sparse_super,large_file','-E',f'nodiscard,lazy_itable_init=0,root_owner={os.getuid()}:{os.getgid()}',self.img],fsize=self.ext);self.mount();s=os.statvfs(self.d);need(s.f_bavail*s.f_frsize>=self.require and s.f_favail>=3200,'real geometry needs more usable guest space/inodes than image');h.timing['quota_setup_s']=clock()-t
    def mount(self):
        h=self.h;need(not self.mountinfo() and self.ident()==self.inode,'changed/unavailable private device');self.num+=1;self.tag=f'volume{self.num}';h.daemons.add(self.tag)
        h.launch(self.tag,[FUSE,'-s','-f','-o','rw,noatime,fsname='+str(self.img),self.img,self.d],data=96<<20,fsize=self.ext);self.daemon=h.work[self.tag];end=clock()+6
        def ready():
            need(clock()<end and self.daemon.returncode is None,'quota mount did not become ready');return self.mountinfo() is not None
        h.until(ready);self.lease=self.owned();self.d.chmod(0o700);s=os.statvfs(self.d);need(s.f_frsize==4096 and s.f_bavail*s.f_frsize<self.ext and self.img.stat().st_blocks*512<=self.ext,'unbounded volume');self.epochs.append(dict(tag=self.tag,mount_id=self.lease,extent=self.ext,free_bytes=s.f_bavail*s.f_frsize,free_inodes=s.f_favail,required_empty_free_bytes=self.require));save(h.d/(self.tag+'.json'),self.epochs[-1])
    def close(self):
        if self.lease is None:return
        from overlap_real_v10_meter import files
        self.owned();self.sealed_bytes=files(self.h.root,self.h.work,self.h.inspect)[0]['guest'][0];self.lease=None;self.h.run(f'unmount{self.num}',[UMOUNT,'-u',self.d]);self.h.until(lambda:self.daemon.returncode is not None);need(self.daemon.returncode==0 and not self.mountinfo() and self.ident()==self.inode,'quota unmount/daemon exit');self.daemon=None
    def check_offline(self):
        need(self.daemon is None and not self.mountinfo() and self.ident()==self.inode,'cannot fsck live or substituted image');self.h.run(f'checkfs{self.num}',[FSCK,'-f','-n',self.img],data=64<<20,fsize=self.ext)
    def reopen_cold(self):
        t=clock();self.close();self.check_offline();cold(self.img);self.mount();self.h.timing['cold_backing_and_restart_s']=clock()-t
    def retire(self):
        self.close();self.check_offline();need(self.ident()==self.inode and self.img.stat().st_nlink==1 and self.daemon is None and not self.mountinfo(),'live image cannot retire');self.img.unlink();self.d.rmdir()
    def preserve(self):
        # Called after cooperative STOP of the row group. Keep the regular
        # image and all original partial evidence; only release our own mount.
        if self.mountinfo():
            self.owned();self.lease=None
            sp.run([UMOUNT,'-u',self.d],check=True,timeout=5,stdout=sp.DEVNULL)
        if self.daemon is not None and self.daemon.returncode is None:
            end=clock()+3
            while self.daemon.returncode is None and clock()<end:self.h.inspect(self.tag,self.daemon);time.sleep(.01)
            if self.daemon.returncode is None:os.killpg(self.daemon.pid,signal.SIGTERM);self.daemon.wait(timeout=2)
        for name,th in self.h.drains:
            if name in self.h.daemons:th.join(2)
        need(not self.mountinfo(),'private filesystem remained mounted on refusal')
