"""No corpus. Validate owned-volume handoff with column-sized files, durable release and offline checks."""
import hashlib
import mmap
import os
from pathlib import Path
import sys
import time
from overlap_real_v10_plan import N,K,U,M,M20,S
from overlap_real_v10_meter import Group,save
from overlap_real_v10_product import private,no_owner,fsyncdir
from overlap_real_v10_volume import Volume


def test(root,role):
    d=root/role;d.mkdir(mode=0o700);h=Group(root,role,int(time.time())+45);v=h.volume=Volume(h,M20 if role=='reference' else 6<<20)
    try:
        v.create();tape=v.d/'generated';other=v.d/'control';length=U+(12 if role=='reference' else 16)*K-8;chunk=bytes(range(256))*1024;digest=hashlib.sha256()
        # Interleave 256 KiB writes with 4 KiB records in a second inode so
        # the large file cannot count on one contiguous sequence of blocks.
        with tape.open('xb',buffering=0) as f,other.open('xb',buffering=0) as g:
            for off in range(0,length,len(chunk)):
                h.guard();buf=chunk[:min(len(chunk),length-off)];assert f.write(buf)==len(buf) and g.write(chunk[:4096])==4096;digest.update(buf)
            os.fsync(f.fileno());os.fsync(g.fileno())
        fsyncdir(v.d);h.sample();v.reopen_cold()
        with tape.open('rb') as f:assert hashlib.file_digest(f,'sha256').digest()==digest.digest()
        assert tape.stat().st_size==length;free=lambda:os.statvfs(v.d).f_bavail*4096;before=free();no_owner(tape,None);after=free();assert after-before>=length and not tape.exists(),'large-file unlink did not give durable space back'
        private_slot=v.d/'slot';canonical=v.d/'published'
        with private_slot.open('x+b') as f:
            f.truncate(2410736)
            with mmap.mmap(f.fileno(),0) as mm:mm[-1]=25;mm.flush()
            os.fsync(f.fileno());os.link(private_slot,canonical);assert private_slot.samefile(canonical) and os.fstat(f.fileno()).st_nlink==2
            try:os.link(private_slot,canonical)
            except FileExistsError:pass
            else:raise AssertionError('slot publication replaced a live file')
            with canonical.open('rb') as reader:reader.seek(-1,2);assert reader.read(1)==b'\x19'
            canonical.unlink();fsyncdir(v.d);assert os.fstat(f.fileno()).st_nlink==1;f.truncate(13);os.fsync(f.fileno())
        no_owner(other,None);no_owner(private_slot,None);v.retire();h.wait();assert h.peaks['outer_charged']<2*N and h.peaks['scratch_bytes']<2*N and set(h.results)=={'volume1','volume2','unmount1','unmount2','checkfs1','checkfs2','mkfs'};print(f'PASS {role}: fresh {length} B interleaved column, hash after checked cold remount, unlink returned {after-before} B; mapped reusable hard link; both read-only fscks and all workers 0')
    except BaseException:
        try:h.abort()
        finally:v.preserve()
        raise
    finally:save(d/'status.json',h.report())

if __name__=='__main__':
    root=Path(sys.argv[1]);private(root);assert root.parent==S and root.name.startswith('real-v10-envelope-')
    for role in ('reference','candidate'):test(root,role)
