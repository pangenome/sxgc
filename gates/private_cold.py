"""Evict only a caller-owned private file and verify residency via Linux mincore."""
import ctypes as C
import mmap
import os
import sys

def cold(path):
    libc=C.CDLL(None,use_errno=True)
    with open(path,'rb') as f:
        os.fsync(f.fileno());os.posix_fadvise(f.fileno(),0,0,os.POSIX_FADV_DONTNEED)
        with mmap.mmap(f.fileno(),0,access=mmap.ACCESS_COPY) as m:
            addr=C.addressof(C.c_char.from_buffer(m)); pages=(len(m)+mmap.PAGESIZE-1)//mmap.PAGESIZE
            v=(C.c_ubyte*pages)()
            if libc.mincore(C.c_void_p(addr), C.c_size_t(len(m)),v): raise OSError(C.get_errno(),'mincore')
            resident=sum(b&1 for b in v)
            if resident: raise RuntimeError(f'{path}: {resident}/{pages} pages resident after eviction')
if __name__=='__main__':
    for path in sys.argv[1:]: cold(path)
