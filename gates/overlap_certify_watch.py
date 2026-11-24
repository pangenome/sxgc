"""Read-only inotify observer for final container certification of an already running matched sweep.

The frozen launcher closes exits.log after both services, invokes certify (interpreter startup,
SHA and member check, writes identity.json), then invokes the score. Neither raw/prepared time
nor oracle CPU/wall should include/add this interval. Never open a corpus, FIFO or container.
"""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import re
import select
import struct
import sys
import time

CLOSE_WRITE=0x8;MOVED_TO=0x80;CREATE=0x100;ISDIR=0x40000000;OVERFLOW=0x4000;IGNORED=0x8000
HEADER=struct.Struct('=iIII');NAMES=('exits.log','identity.json')

def need(ok,msg):
    if not ok:raise ValueError(msg)

def bounded(d,name):
    p=d/name;st=p.lstat();need(p.is_file() and not p.is_symlink() and st.st_size<8192,'unexpected receipt inode')
    b=p.read_bytes();return (st.st_mtime_ns,hashlib.sha256(b).hexdigest(),b)

def observe(sandbox,first,profiles,dest,deadline,ready=None):
    need(first.is_dir() and first.resolve()==first and first.parent==sandbox and len(profiles)==len(set(profiles)) and profiles and
         all(re.fullmatch('packed-10g-l[124]-b[01]-t[124]-r[01]',p) for p in profiles) and re.fullmatch(profiles[0]+r'-[0-9a-zA-Z]{6}',first.name),'expected exact matched run directory')
    need(dest.is_dir() and not list(dest.iterdir()) and not dest.resolve().is_relative_to(sandbox),'need fresh separate destination')
    libc=C.CDLL(None,use_errno=True);libc.inotify_add_watch.argtypes=[C.c_int,C.c_char_p,C.c_uint32];fd=libc.inotify_init1(os.O_NONBLOCK|os.O_CLOEXEC)
    if fd<0:raise OSError(C.get_errno(),'inotify_init1')
    watched={};byprof={};marks={};finished=set()
    def add(p,mask):
        wd=libc.inotify_add_watch(fd,os.fsencode(p),mask)
        if wd<0:raise OSError(C.get_errno(),str(p))
        return wd
    def match(d):
        for profile in profiles:
            if re.fullmatch(profile+r'-[0-9a-zA-Z]{6}',d.name):return profile
    def watch(d,profile):
        need(d==d.resolve() and d.parent==sandbox and d.is_dir() and profile not in byprof,'ambiguous arm or non-directory')
        wd=add(d,CLOSE_WRITE);need(wd not in watched,'duplicate watch');watched[wd]=d;byprof[profile]=d;marks[d]={}
        need(not any((d/name).exists() for name in NAMES),'late observer: do not reconstruct a monotonic time')
    try:
        parent=add(sandbox,CREATE|MOVED_TO);watch(first,profiles[0]);
        if ready is not None:ready()
        while len(finished)<len(profiles):
            left=deadline-time.time();need(left>0,'deadline: no further observation');avail,_,_=select.select([fd],[],[],min(60,left))
            if not avail:continue
            buf=os.read(fd,65536);i=0
            while i<len(buf):
                wd,mask,_,sz=HEADER.unpack_from(buf,i);name=buf[i+HEADER.size:i+HEADER.size+sz].split(b'\0',1)[0].decode(errors='surrogateescape');i+=HEADER.size+sz
                need(not mask&(OVERFLOW|IGNORED),'watch lost');stamp=time.monotonic_ns()
                if wd==parent and mask&ISDIR:
                    d=sandbox/name;profile=match(d)
                    if profile is not None:watch(d,profile)
                elif wd in watched and mask&CLOSE_WRITE and name in NAMES:
                    d=watched[wd];m=marks[d];need(name not in m and (name==NAMES[0])==(not m),'out of order or duplicated close')
                    m[name]=(stamp,*bounded(d,name)[:2]);
                    if name==NAMES[-1]:
                        def interval(i):return (m[NAMES[1]][i]-m[NAMES[0]][i])/1e9
                        data=dict(kind='post-service certification envelope; not SHA kernel time',source=str(d),profile=match(d),
                            observed_close_to_close_s=interval(0),filesystem_mtime_delta_s=interval(1),close_events=m)
                        need(min(interval(0),interval(1))>=0,'negative clock');p=dest/(data['profile']+'.json')
                        with p.open('x') as f:json.dump(data,f,indent=2,sort_keys=True);f.write('\n')
                        finished.add(d);print(f"CERTIFICATION_OBSERVED {data['profile']} close_s={interval(0):.6f} mtime_delta_s={interval(1):.6f}",flush=True)
    finally:os.close(fd)

if __name__=='__main__':
    first,dest=map(Path,sys.argv[1:3]);deadline=int(sys.argv[3]);observe(first.parent,first,sys.argv[4:],dest,deadline,lambda:print('WATCHING_CERTIFICATION',first,dest,flush=True))
