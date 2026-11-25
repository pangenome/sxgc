"""Fresh v10 construction. Invoked by the installed `xsa build-stream`."""
import argparse
import os
from pathlib import Path
import re
import signal
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


def main(argv):
    tools, xsa = Path(argv[0]), argv[1]
    cli = argparse.ArgumentParser(prog='xsa build-stream', description='Build a document-aware v10 suffix index from an immutable raw corpus.')
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
    with tempfile.TemporaryDirectory(prefix='.xsa-stream-', dir=parent) as tmp, \
         tempfile.TemporaryDirectory(prefix='.xsa-publish-', dir=a.output.parent) as stage:
        tmp = Path(tmp)
        cols = tmp/'columns'; cols.mkdir()
        tape, refs, hist, inv = (cols/name for name in ('parse.u64','phrases.ref','histogram','inverse'))
        index = Path(stage)/'index'
        phases = []
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
                phases.append(elapsed)
                if a.verbose:
                    print(f'build-stream: {name} {elapsed:.3f}s',file=sys.stderr,flush=True)
            run('parse',[[tools/'pfp_parse',raw,cols,workers,mem,str(budget-reserve)]])
            # The bitmap can open before the row producer retires its inputs.
            bitmap = (n+8)//8
            if 2*tape.stat().st_size+refs.stat().st_size+2048+bitmap > budget-reserve:
                raise RuntimeError('PFP columns, inverse and v10 bitmap exceed scratch budget')
            run('rank',[[tools/'stream_rank','inverse',tape,inv,workers,mem]])
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
            print(f'build-stream: {a.output} ({a.output.stat().st_size} bytes); raw-to-prepared {sum(phases[:2]):.3f}s',file=sys.stderr)


def interrupted(sig,frame):
    raise SystemExit(128+sig)


if __name__ == '__main__':
    for sig in (signal.SIGINT,signal.SIGTERM,signal.SIGHUP):
        signal.signal(sig,interrupted)
    try:
        main(sys.argv[1:])
    except (OSError,RuntimeError) as e:
        sys.exit(f'build-stream: {e}')
