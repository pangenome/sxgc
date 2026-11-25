"""Unprivileged private fixed-capacity filesystem test. No corpus, row worker or index."""
import ctypes
import errno
import hashlib
import mmap
import os
from pathlib import Path
import random
import struct
import stat
import subprocess as sp
import sys
import time
from private_cold import cold


def main(d,front=None):
    assert d==d.resolve() and d.parent==Path('/tmp') and d.name.startswith('real-v10-fs-test-') and stat.S_IMODE(d.stat().st_mode)==0o700
    cg=Path('/sys/fs/cgroup'+Path('/proc/self/cgroup').read_text().split('::')[1].strip());assert (cg/'memory.max').read_text().strip()==str(96<<20) and (cg/'memory.swap.max').read_text().strip()=='0'
    img=d/'fixed.img';mnt=d/'private';mnt.mkdir(mode=0o700);size=16<<20
    with img.open('xb') as f:os.posix_fallocate(f.fileno(),0,size);f.flush();os.fsync(f.fileno());assert os.fstat(f.fileno()).st_blocks*512==size
    sp.run(['/usr/sbin/mkfs.ext4','-q','-F','-b','4096','-m','0','-O','^has_journal','-E',f'nodiscard,lazy_itable_init=0,root_owner={os.getuid()}:{os.getgid()}',img],check=True,timeout=8)
    p=sp.Popen(['/usr/bin/fuse2fs','-s','-f','-o','rw,noatime,fsname='+str(img),img,mnt],stdout=sys.stderr);end=time.monotonic()+5
    try:
        while not os.path.ismount(mnt):
            if p.poll() is not None:raise RuntimeError('unprivileged private fuse2fs mount refused, rc='+str(p.returncode))
            assert time.monotonic()<end;time.sleep(.03)
        # The real protocol requires private owner, durable fsync, no-replace links,
        # nlink after unlink, and Linux cold/mincore. A mount alone proves nothing.
        mnt.chmod(0o700);s=mnt/'slot';t=mnt/'canonical';payload=b'abcdefgh'*32768
        with s.open('x+b') as f:
            f.write(payload);f.flush();os.fsync(f.fileno());os.link(s,t);assert os.fstat(f.fileno()).st_nlink==2 and s.samefile(t);t.unlink();assert os.fstat(f.fileno()).st_nlink==1
        cold(s);assert hashlib.sha256(s.read_bytes()).digest()==hashlib.sha256(payload).digest()
        with s.open('r+b') as f,mmap.mmap(f.fileno(),0) as m:m[0]=1;m.flush();os.fsync(f.fileno())
        assert s.read_bytes()[0]==1
        a=mnt/'private.partial';b=mnt/'columns';a.mkdir();libc=ctypes.CDLL(None,use_errno=True);mv=lambda:libc.renameat2(-100,os.fsencode(a),-100,os.fsencode(b),1)
        ok=mv()==0
        if ok:
            a.mkdir();assert mv()!=0 and ctypes.get_errno()==errno.EEXIST;print('RENAME_NOREPLACE directories supported')
        else:print('RENAME_NOREPLACE directories unsupported, errno='+str(ctypes.get_errno())+'; original position front cannot publish here')
        line,=[s for s in Path('/proc/self/mountinfo').read_text().splitlines() if s.split()[4]==str(mnt)];print('private mount:',line)
        if front is not None:
            src=d/'synthetic';src.write_bytes(random.Random(5756).randbytes(65536));old=Path('/mnt/nvme2n1/erikg/TAPEREADER/sandbox/pos64-front-10g-XTNbSv/front');new=mnt/'ready';native=d/'native';t=str(int(time.time())+30)
            def cmd(exe,mode,out,cap):return sp.run([exe,mode,src,out,'1',str(16<<20),str(cap),d/'STOP',t],capture_output=True,timeout=5)
            assert cmd(old,'pos',native,131072).returncode==0
            total=sum(f.stat().st_size for f in native.iterdir())+48
            assert cmd(front,'pos-manifest',new,total-1).returncode==75 and not new.exists() and not (mnt/'ready.partial').exists()
            assert cmd(front,'pos-manifest',new,total).returncode==0 and cmd(front,'pos-manifest',new,total).returncode==75;data=mnt/'ready.partial';assert new.samefile(data/'manifest') and new.stat().st_nlink==2 and struct.unpack('<6Q',new.read_bytes())[0:2]==(0x3143465058,65536)
            assert all((data/f.name).read_bytes()==f.read_bytes() for f in native.iterdir());print('PASS manifest cap/refusal, exclusive publication and byte-identical columns on quota FS')
        fail=mnt/'exceed';block=bytes(65536)
        try:
            with fail.open('xb',buffering=0) as f:
                for _ in range(300):f.write(block)
                os.fsync(f.fileno())
        except OSError as e:assert e.errno==28,e
        else:raise RuntimeError('missing capacity fence')
        assert img.stat().st_size==size and img.stat().st_blocks*512==size;print('PASS private FS exact ENOSPC, fsync, cold/mincore, reopen and hard-link credit')
    finally:
        if os.path.ismount(mnt):sp.run(['/usr/bin/fusermount3','-u',mnt],check=True,timeout=4)
        if p.poll() is None:
            try:p.wait(timeout=3)
            except sp.TimeoutExpired:p.terminate();p.wait(timeout=2)
if __name__=='__main__':main(Path(sys.argv[1]),Path(sys.argv[2]) if len(sys.argv)==3 else None)
