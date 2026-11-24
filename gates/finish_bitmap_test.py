"""Exercise the frozen binary's open/unlink ABI, incl. exclusive creation.
Test the placement adapter, *not* a substitute for a certified 3 GB product.
"""
import ctypes as c
import errno
import mmap
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def worker():
    api=c.CDLL(None,use_errno=True); create=os.O_CREAT|os.O_EXCL|os.O_RDWR
    p=Path(os.environ['XSA_GATE_BITMAP_PATH']); bitmap=bytes(p); other=bytes(p.with_suffix('.lcpruns'))
    ram=int(os.environ['XSA_GATE_BITMAP_DIRFD'])
    # A preexisting disk source cannot be clobbered or silently remapped.
    parent=os.open(p.parent,os.O_DIRECTORY); existing=os.open(p.name,create,0o600,dir_fd=parent)
    os.write(existing,b'banked'); assert api.open(bitmap,create,0o600)==-1 and c.get_errno()==errno.EEXIST
    assert os.pread(existing,6,0)==b'banked'; os.close(existing); os.unlink(p.name,dir_fd=parent); os.close(parent)
    fd=api.open(bitmap,create,0o600); assert fd>=0 and os.fstat(fd).st_dev==os.fstat(ram).st_dev
    assert not p.exists() and api.open(bitmap,create,0o600)==-1 and c.get_errno()==errno.EEXIST
    assert os.stat('frag.chi-bitmap',dir_fd=ram).st_ino==os.fstat(fd).st_ino
    os.ftruncate(fd,8192)
    with mmap.mmap(fd,8192) as view:
        view[7999]=255; assert os.pread(fd,1,7999)==b'\xff'
    os.close(fd)
    # All other files follow normal backing, including no-mode read-only open.
    fd=api.open(other,create,0o600); assert fd>=0 and os.fstat(fd).st_dev==p.parent.stat().st_dev
    os.write(fd,b'spool'); os.close(fd); fd=api.open(other,os.O_RDONLY); assert os.read(fd,5)==b'spool'; os.close(fd)
    assert api.unlink(other)==api.unlink(bitmap)==0 and not os.listdir(ram)


def main(adapter):
    with tempfile.TemporaryDirectory() as d, tempfile.TemporaryDirectory(dir='/dev/shm') as ram:
        disk=Path(d)/'merged'; disk.mkdir(); fd=os.open(ram,os.O_DIRECTORY)
        try:
            env=dict(os.environ,LD_PRELOAD=str(adapter.resolve()),XSA_GATE_BITMAP_PATH=str(disk/'frag.chi-bitmap'),XSA_GATE_BITMAP_DIRFD=str(fd))
            p=subprocess.run([sys.executable,__file__,'--worker'],env=env,pass_fds=(fd,),capture_output=True)
            assert p.returncode==0 and p.stderr.splitlines()==[b'GATE_BITMAP create=tmpfs exclusive=1',b'GATE_BITMAP unlink=tmpfs'],p.stderr
            # Refuse missing/invalid target fd before any writes.
            p=subprocess.run(['/bin/true'],env=dict(env,XSA_GATE_BITMAP_DIRFD='-1'),capture_output=True); assert p.returncode==70
            assert not list(disk.iterdir()) and not os.listdir(ram)
        finally: os.close(fd)
    print('PASS exclusive private tmpfs bitmap, mmap visibility, unchanged neighbor, cleanup and invalid-fd refusal')

if __name__=='__main__':
    if sys.argv[1]=='--worker':worker()
    else:main(Path(sys.argv[1]))
