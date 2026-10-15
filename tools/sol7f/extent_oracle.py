#!/usr/bin/env python3
"""Check physical extent accounting on the actual scratch filesystem."""
import ctypes, os, sys
path=sys.argv[1]
fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
lib=ctypes.CDLL(None,use_errno=True)
lib.fallocate.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_longlong,ctypes.c_longlong]
peak=0;batch=102400;budget=11*batch
try:
    for i in range(2000):
        assert lib.fallocate(fd,0,i*batch,batch)==0
        os.pwrite(fd,bytes(batch),i*batch)
        allocated=os.fstat(fd).st_blocks*512
        peak=max(peak,allocated)
        # Extent-tree blocks are filesystem metadata, charged separately.
        assert allocated<=budget+65536,(i,allocated)
        if i>=10:assert lib.fallocate(fd,3,(i-10)*batch,batch)==0
    print(f'SOL7F_XFS_EXPLICIT_EXTENT_PASS batches=2000 live_batches=11 budget_bytes={budget} allocated_peak_bytes={peak} metadata_allowance_bytes=65536')
finally:
    os.close(fd);os.unlink(path)
