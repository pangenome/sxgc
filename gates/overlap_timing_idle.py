"""Refuse simultaneous known timed arms. Use in addition to the series flock.
No host-wide idleness or exclusivity claim. A kernel-frozen cgroup cannot run;
a missing/unreadable cgroup.events does NOT count as frozen.
"""
import os
from pathlib import Path

WORKERS = {'overlap_resident', 'pfp_sort_r', 'xsa_finish_cur', 'overlap_resident_gate.py',
           'overlap_ten_product.sh', 'overlap_radix_product.sh', 'overlap_resident_gate.sh',
           'overlap_radix_stage.sh', 'overlap_three_gate.sh', 'overlap_sxls_pair.sh',
           'overlap_ten_front.sh', 'cache_pipeline.py'}

def frozen(proc):
    try:
        v, ctr, path = (proc/'cgroup').read_text().strip().split(':',2)
        if v != '0' or ctr or not path.startswith('/'): return False
        return 'frozen 1' in (Path('/sys/fs/cgroup')/path.lstrip('/')/'cgroup.events').read_text().splitlines()
    except (OSError, ValueError): return False

def exclusive(own_pid=None):
    ignore={own_pid}; pid=os.getpid()
    # The coordinator and its wrapper need not change names to avoid detecting
    # themselves. Siblings and systemd workers are deliberately NOT excluded.
    while pid>1 and pid not in ignore:
        ignore.add(pid)
        try: pid=int(Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[1])
        except (OSError,ValueError,IndexError): break
    for cmd in Path('/proc').glob('[0-9]*/cmdline'):
        if int(cmd.parent.name) in ignore:continue
        try:argv=cmd.read_bytes().split(b'\0')
        except (FileNotFoundError, PermissionError, ProcessLookupError):continue
        # Only actual executable/script arguments, not substring matches inside
        # an editor, a log viewer or a waiter script's source/arguments.
        names=[os.path.basename(os.fsdecode(x)) for x in argv[:3]]
        if WORKERS.intersection(names) and not frozen(cmd.parent):
            raise RuntimeError(f'other active timing process: {cmd.parent.name} {names}')

if __name__=='__main__':exclusive()
