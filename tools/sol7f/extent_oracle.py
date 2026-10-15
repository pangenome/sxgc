#!/usr/bin/env python3
"""Check physical extent accounting on the actual scratch filesystem."""
import ctypes, os, sys
path=sys.argv[1]
fd=os.open(path,os.O_CREAT|os.O_EXCL|os.O_RDWR,0o600)
lib=ctypes.CDLL(None,use_errno=True)
lib.fallocate.argtypes=[ctypes.c_int,ctypes.c_int,ctypes.c_longlong,ctypes.c_longlong]
peak=0;slot=106496;live=[];charged=0;budget=11*slot
try:
    for i in range(2000):
        charge=[4096,12288,24576,106496][i%4]
        assert lib.fallocate(fd,0,i*slot,charge)==0
        os.pwrite(fd,bytes(charge),i*slot)
        live.append((i*slot,charge)); charged+=charge
        allocated=os.fstat(fd).st_blocks*512
        peak=max(peak,allocated)
        # Extent-tree blocks are filesystem metadata, charged separately.
        assert allocated<=charged+65536 and charged<=budget,(i,allocated,charged)
        if len(live)==11:
            offset,length=live.pop(0)
            assert lib.fallocate(fd,3,offset,length)==0
            charged-=length
    print(f'SOL7F_XFS_EXPLICIT_EXTENT_PASS batches=2000 live_batches=11 variable_page_charges=1 fixed_slot_bytes=106496 budget_bytes={budget} allocated_peak_bytes={peak} metadata_allowance_bytes=65536')
finally:
    os.close(fd);os.unlink(path)
