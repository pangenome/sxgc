"""Fresh v10 construction. Invoked by the installed `xsa build-pfp`."""
import argparse
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time


def byte_size(value):
    m = re.fullmatch(r'([1-9][0-9]*)([KMGTP]?)', value.upper())
    if not m:
        raise argparse.ArgumentTypeError('expected bytes or a K/M/G/T/P binary suffix')
    n = int(m[1]) << (10 * ' KMGTP'.index(m[2] or ' '))
    if n >= 1 << 63:
        raise argparse.ArgumentTypeError('size exceeds 64-bit file offsets')
    return n


def pipeline(commands, log):
    """Pipe bounded kernel buffers only. Any failed stage cancels its peers."""
    procs = {}
    previous = None
    try:
        for i, cmd in enumerate(commands):
            p = subprocess.Popen(cmd, stdin=previous or subprocess.DEVNULL,
                                 stdout=subprocess.PIPE if i+1 < len(commands) else log,
                                 stderr=log)
            procs[p.pid] = p
            if previous is not None:
                previous.close()  # downstream failure must reach the writer as EPIPE
            previous = p.stdout
        while procs:
            pid, status = os.waitpid(-1, 0)
            p = procs.pop(pid)
            p.returncode = os.waitstatus_to_exitcode(status)
            if p.returncode:
                raise RuntimeError(f'{Path(p.args[0]).name} {p.args[1]} exited {p.returncode}')
    finally:
        if previous is not None:
            previous.close()
        for p in procs.values():
            p.kill()
        for p in procs.values():
            p.wait()


def rank_tape(tools, tape, alphabet, inverse, workers, memory, free, run, note, level=1):
    """One seam: P (LE u64 IDs) -> one-based inverse(P), END rank zero.

    `free` excludes *all* live ancestor columns. The child directory lives
    until the lift is over: references point into P, never into copied text.
    """
    size = tape.stat().st_size
    n = size // 8
    if not 0 < alphabet <= n or size != 8*n:
        raise RuntimeError('invalid lexical ID tape')
    if size > free:
        raise RuntimeError(f'P{level} inverse needs {size} bytes, {free} free')
    ranker = tools/'stream_rank'
    plan = subprocess.run([ranker, 'plan', str(n), workers], capture_output=True)
    match = re.fullmatch(rb'parse64_array_bound=(\d+)\n', plan.stdout)
    if plan.returncode or not match:
        raise RuntimeError(f'invalid P{level} kernel plan: {plan.stderr[-512:]!r}')
    required = int(match[1]); kernel_fits = required <= int(memory)

    # No repeated first symbol means no repeated block, and tiny tapes are
    # cheaper as leaves. Otherwise measure the ACTUAL next dictionary. A
    # shorter Q alone is not a work saving: count owned offsets as well.
    if n >= 16 and (not kernel_fits or alphabet < n):
        with tempfile.TemporaryDirectory(prefix='level-', dir=inverse.parent) as name:
            child = Path(name); q = child/'parse.u64'; qi = child/'inverse'
            run(f'parse P{level}', [[tools/'pfp_parse', 'ids', tape, child, workers, memory, str(free)]])
            lower, m, k, owned, _ = struct.unpack('<5Q', (child/'census').read_bytes())
            child_bytes = 8*m + 24*k + 40
            if not (lower == n and 0 < k <= m <= n and k <= owned <= n
                    and q.stat().st_size == 8*m and (child/'phrases.ref').stat().st_size == 24*k):
                raise RuntimeError('bad recursive parse census')
            lift_bytes = child_bytes + 8*m + size + 16*owned
            use = (m >= 2 and 2*m <= n and (not kernel_fits or owned+m < n)
                   and lift_bytes <= free)
            note(f'PFP_CENSUS level={level} lower={n} upper={m} words={k} owned={owned} '
                 f'direct_bytes={required} lift_scratch={lift_bytes} free={free} descend={int(use)}')
            if use:
                rank_tape(tools, q, k, qi, workers, memory, free-child_bytes, run, note, level+1)
                run(f'lift P{level+1}->P{level}', [[tools/'pfp_lift', tape, child, inverse, workers,
                                                    memory, str(free-child_bytes-8*m)]])
                return
            if not kernel_fits:
                raise RuntimeError(f'P{level} direct rank needs {required} array bytes; next census '
                                   f'{n}->{m}, lift needs {lift_bytes} scratch bytes ({free} free)')
        # Unselected trial retired BEFORE allocating the direct inverse.
    if not kernel_fits:
        raise RuntimeError(f'P{level} direct rank needs {required} array bytes, grant {memory}')
    note(f'PFP_LEAF level={level} symbols={n} alphabet={alphabet} array_bound={required}')
    run(f'rank P{level}', [[ranker, 'inverse', tape, inverse, workers, memory]])


def main(argv):
    tools, xsa = Path(argv[0]), argv[1]
    cli = argparse.ArgumentParser(prog='xsa build-pfp', description='Build a document-aware v10 suffix index by prefix-free parsing.')
    cli.add_argument('corpus', type=Path)
    cli.add_argument('--docs-map', required=True, type=Path, metavar='DIR', help='XSEP map of the corpus (or of the same stream before prefix truncation)')
    cli.add_argument('-o','--output', required=True, type=Path, metavar='INDEX', help='new index; never overwrites')
    cli.add_argument('-T','--tmpdir', type=Path, help='scratch parent (default: TMPDIR or output directory)')
    cli.add_argument('-j','--threads', type=int, default=min(4,len(os.sched_getaffinity(0))), help='sort workers (default: up to 4)')
    cli.add_argument('-m','--sort-memory', type=byte_size, help='sort array budget, not total RSS (default: max(256M, 2 x corpus bytes))')
    cli.add_argument('-v','--verbose', action='store_true', help='print stage diagnostics and phase times to stderr')
    a = cli.parse_args(argv[2:])
    raw = a.corpus.resolve(strict=True)
    n = raw.stat().st_size
    if not raw.is_file() or not 0 < n < 1<<43:
        cli.error('corpus must be a nonempty regular file shorter than 2^43 bytes')
    if not (a.docs_map/'seps.bin').is_file():
        cli.error('--docs-map must contain seps.bin')
    if not 1 <= a.threads <= min(64,len(os.sched_getaffinity(0))):
        cli.error('thread count exceeds available CPUs or the 64-worker limit')
    if os.path.lexists(a.output):
        cli.error(f'output exists: {a.output}')
    mem = str(a.sort_memory or max(256<<20,2*n))
    workers = str(a.threads)
    # Logical planning, NOT a filesystem quota. Include the open key/inverse
    # pair, bitmap, histogram and room for allocation rounding on native files.
    budget = max(65536,2*n)
    reserve = max(16384,n//8)
    parent = a.tmpdir or os.environ.get('TMPDIR') or a.output.parent
    # This path never resumes or imports experimental finish settings.
    os.environ.pop('XSA_FINISH_SPOOL',None)
    with tempfile.TemporaryDirectory(prefix='.xsa-pfp-', dir=parent) as tmp, \
         tempfile.TemporaryDirectory(prefix='.xsa-publish-', dir=a.output.parent) as stage:
        tmp = Path(tmp)
        cols = tmp/'columns'; cols.mkdir()
        tape, refs, hist, inv = (cols/name for name in ('parse.u64','phrases.ref','histogram','inverse'))
        index = Path(stage)/'index'
        with tempfile.TemporaryFile(dir=tmp) as errors:
            log = sys.stderr if a.verbose else errors
            def run(name, commands):
                start = time.monotonic()
                try:
                    pipeline(commands,log)
                except RuntimeError:
                    if not a.verbose:
                        errors.seek(max(0,errors.tell()-4096))
                        sys.stderr.buffer.write(errors.read())
                    raise
                elapsed = time.monotonic()-start
                note(f'build-pfp: {name} {elapsed:.3f}s')
            def note(message):
                if a.verbose:
                    print(message,file=sys.stderr,flush=True)
            start = time.monotonic()
            run('parse',[[tools/'pfp_parse',raw,cols,workers,mem,str(budget-reserve)]])
            # The bitmap can open before the row producer retires its inputs.
            bitmap = (n+8)//8
            if 2*tape.stat().st_size+refs.stat().st_size+2048+bitmap > budget-reserve:
                raise RuntimeError('PFP columns, inverse and v10 bitmap exceed scratch budget')
            free = budget-reserve-tape.stat().st_size-refs.stat().st_size-hist.stat().st_size
            rank_tape(tools, tape, refs.stat().st_size//24, inv, workers, mem, free, run, note)
            prepared = time.monotonic()-start
            # pfp_rows closes AND unlinks tape, inverse and refs before opening
            # its posting file. Count the histogram and simultaneous chi bitmap.
            cap = budget-reserve-2048-bitmap
            run('stream+finish',[
                [tools/'pfp_rows',raw,cols,workers,mem,str(cap)],
                [xsa,'stream-context',raw,a.docs_map],
                [tools/'xsa_finish','--from-rows',hist,tmp/'stream',index],
            ])
        # Same filesystem, atomic no-replace. No partial product at the public
        # name, even if an upstream worker fails after emitting its last packet.
        os.link(index,a.output)
        dfd = os.open(a.output.parent,os.O_RDONLY|os.O_DIRECTORY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
        if a.verbose:
            print(f'build-pfp: {a.output} ({a.output.stat().st_size} bytes); raw-to-prepared {prepared:.3f}s',file=sys.stderr)


def interrupted(sig,frame):
    raise SystemExit(128+sig)


if __name__ == '__main__':
    for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP):
        signal.signal(sig,interrupted)
    try:
        main(sys.argv[1:])
    except (OSError,RuntimeError) as e:
        sys.exit(f'build-pfp: {e}')
