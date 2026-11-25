"""Refuse simultaneous known timed arms. Use in addition to the series flock.
No host-wide idleness or exclusivity claim. A kernel-frozen cgroup cannot run;
a missing/unreadable cgroup.events does NOT count as frozen.
"""
import os
import re
from pathlib import Path

WORKERS = {'overlap_resident', 'pfp_sort_r', 'xsa_finish_cur', 'overlap_resident_gate.py',
           'overlap_ten_product.sh', 'overlap_radix_product.sh', 'overlap_resident_gate.sh',
           'overlap_radix_stage.sh', 'overlap_three_gate.sh', 'overlap_sxls_pair.sh',
           'overlap_ten_front.sh', 'cache_pipeline.py', 'xsa_finish',
           'overlap_real_v10_product.sh', 'overlap_real_v10_product.py'}

def frozen(proc):
    try:
        v, ctr, path = (proc/'cgroup').read_text().strip().split(':',2)
        if v != '0' or ctr or not path.startswith('/'): return False
        return 'frozen 1' in (Path('/sys/fs/cgroup')/path.lstrip('/')/'cgroup.events').read_text().splitlines()
    except (OSError, ValueError): return False

def worker_name(argv):
    # Inspect the native executable, or the *script operand* of an interpreter.
    # basename() of an entire `bash -c '.../overlap_resident'` code string is
    # not a process identity. Its actual children are inspected independently.
    if not argv:return None
    exe=os.path.basename(os.fsdecode(argv[0]));args=[os.fsdecode(x) for x in argv[1:] if x]
    if exe in WORKERS:return exe
    shell=exe in ('bash','sh','dash')
    if not shell and not re.fullmatch(r'python(?:3(?:\.\d+)?)?',exe):return None
    it=iter(args)
    for arg in it:
        if arg=='--':arg=next(it,'')
        elif arg=='-':return None
        elif arg.startswith('-'):
            if arg in (('-o','-O','--init-file','--rcfile') if shell else ('-W','-X','--check-hash-based-pycs')):next(it,None);continue
            if not arg.startswith('--') and ('c' in arg or shell and 's' in arg):return None
            if not shell and arg=='-m':
                name=next(it,'').rsplit('.',1)[-1]+'.py';return name if name in WORKERS else None
            continue
        name=os.path.basename(arg);return name if name in WORKERS else None
    return None

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
        worker=worker_name(argv)
        if worker and not frozen(cmd.parent):
            raise RuntimeError(f'other active timing process: {cmd.parent.name} {worker}')

if __name__=='__main__':exclusive()
