"""No corpus. Exercise the actual supervisor, owned mount transitions and cold backing handoff."""
import os
from pathlib import Path
import sys
from overlap_real_v10_plan import N,S
from overlap_real_v10_meter import Group,save,clock
from overlap_real_v10_product import private
from overlap_real_v10_volume import Volume
import time

def main(root):
    private(root);assert root.parent==S and root.name.startswith('real-v10-envelope-');role='candidate';d=root/role;d.mkdir(mode=0o700);h=Group(root,role,int(time.time())+30);v=h.volume=Volume(h,6<<20)
    try:
        v.create();slot=v.d/'generated';slot.write_bytes(bytes(1<<20));h.sample();v.reopen_cold();assert slot.stat().st_size==1<<20 and slot.read_bytes()==bytes(1<<20);h.sample();v.retire();h.wait();assert h.peaks['outer_charged']<2*N and h.peaks['scratch_bytes']<2<<20 and set(h.results)=={'volume1','volume2','unmount1','unmount2','mkfs'};print('PASS: preallocated NVMe, exact guest fit, cold offline backing, remount and retirement; all helpers 0')
    except BaseException:
        h.abort();v.preserve();raise
    finally:save(d/'status.json',h.report())
if __name__=='__main__':main(Path(sys.argv[1]))
