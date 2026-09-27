#!/usr/bin/env python3
"""Fresh PFP endpoint build, with timed stages and atomic gated publication.

Collection contract: T = s1 0x1E ... sk 0x1E, one cyclic byte string.
0x1E is reserved and must never occur in sequence content; --text is passed as-is.
AGC preparation uses agc2flat --revlines --upper (one reversed contig/record).
FASTA/FASTQ extract already oriented sequences verbatim, separated and terminated by 0x1E.
Scratch defaults beside the input, on the same filesystem; retained for audit.
"""
import argparse
import json
import os
import pathlib
import re
import resource
import shlex
import subprocess
import struct
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]

def main():
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group(required=True)
    for kind in ('text', 'agc', 'fasta', 'fastq'):
        source.add_argument('--' + kind)
    p.add_argument('--output', required=True)
    p.add_argument('--threads', type=int, default=8)
    p.add_argument('--scratch')
    p.add_argument('--log-dir', default=str(ROOT / 'bit6/sxi_logs'))
    p.add_argument('--expect-chi', type=int)
    p.add_argument('--mode', choices=['auto', 'dna', 'text'], default='auto')
    p.add_argument('--verify-text-sample', type=int, default=0)
    p.add_argument('--expect-heads', help='acceptance oracle only; checked after fresh construction')
    p.add_argument('--expect-ri4', help='acceptance oracle only; exact runs and packed-tail byte gate')
    p.add_argument('--verbose', action='store_true')
    p.add_argument('--xsa', default=str(ROOT / 'xsa/target/release/xsa'))
    a = p.parse_args()
    if not 0 <= a.verify_text_sample <= 100000:
        p.error('--verify-text-sample must be 0..100000')
    if not 1 <= a.threads <= 64:
        p.error('--threads must be 1..64')
    source_path = pathlib.Path(a.text or a.agc or a.fasta or a.fastq).absolute()
    out = pathlib.Path(a.output).absolute()
    if os.path.lexists(out):
        p.error('output exists: ' + str(out))
    if not source_path.is_file():
        p.error('input must be a regular file')
    tools = pathlib.Path(os.environ.get('XSA_TOOLS', '/tmp/laneV/tools'))
    pfp = os.environ.get('XSA_PFP', '/home/erikg/pfp/build/pfp++')
    rpf = os.environ.get('XSA_RPFBWT', '/tmp/rpfbwt-sxgc/build-sxgc/rpfbwt')
    agc = os.environ.get('XSA_AGC2FLAT', str(tools / 'agc2flat'))
    required = [pfp, rpf, a.xsa, '/usr/bin/time'] + [str(tools / s) for s in ('rpfbwt_endpoints', 'slim_dump', 'sxi_write')]
    if a.verify_text_sample:
        required.append(str(tools / "sxi_text_audit"))
    if a.agc:
        required.append(agc)
    for tool in required:
        if not os.access(tool, os.X_OK):
            p.error('missing executable: ' + tool)
    scratch = pathlib.Path(a.scratch).absolute() if a.scratch else source_path.parent
    scratch.mkdir(parents=True, exist_ok=True)
    if scratch.stat().st_dev != source_path.stat().st_dev:
        p.error('scratch must be on the same filesystem as the source')
    work = pathlib.Path(tempfile.mkdtemp(prefix='xsa-build-', dir=scratch))
    logs = pathlib.Path(a.log_dir).absolute()
    logs.mkdir(parents=True, exist_ok=True)
    label = out.stem + '-' + work.name
    journal = logs / (label + '.jsonl')
    env = dict(os.environ, OMP_NUM_THREADS=str(a.threads), TMPDIR=str(work))
    # Sequential children; conservative address-space ceiling also bounds RSS.
    _, hard_limit = resource.getrlimit(resource.RLIMIT_AS)
    limit = 149_000_000_000 if hard_limit == resource.RLIM_INFINITY else min(hard_limit, 149_000_000_000)
    resource.setrlimit(resource.RLIMIT_AS, (limit, limit))
    def record(value):
        with journal.open('a') as f:
            f.write(json.dumps(value) + '\n')
    def run(name, command, stdout=None):
        command = list(map(str, command))
        logfile = logs / (label + '.' + name + '.log')
        timing = logs / (label + '.' + name + '.time')
        record(dict(stage=name, command=command, work=str(work), start=time.time()))
        if a.verbose:
            print(name + ': ' + shlex.join(command), file=sys.stderr, flush=True)
        begin = time.monotonic()
        with logfile.open('wb') as log:
            proc = subprocess.run(['/usr/bin/time', '-v', '-o', str(timing), *command],
                                  stdout=stdout or log, stderr=log, env=env)
        measurement = timing.read_text()
        rss = re.search(r'Maximum resident set size \(kbytes\): (\d+)', measurement)
        record(dict(stage=name, returncode=proc.returncode, wall_seconds=time.monotonic()-begin,
                    peak_rss_kib=int(rss[1]) if rss else None, log=str(logfile), timing=str(timing)))
        if proc.returncode:
            raise RuntimeError(f'{name} failed ({proc.returncode}); see {logfile}')
        return logfile
    print(f'xsa build: work={work} log={journal}', file=sys.stderr, flush=True)
    text = source_path
    names = None
    if a.fasta or a.fastq:
        from sxi_sequence_input import prepare
        kind = 'fasta' if a.fasta else 'fastq'
        stage = 'prepare-' + kind
        text = work / 'collection.txt'
        names = work / 'collection.txt.names.tsv'
        begin = time.monotonic()
        record(dict(stage=stage, source=str(source_path), work=str(work), start=time.time()))
        def progress(value):
            record(dict(stage=stage, **value))
            print(f'{stage}: record={value["record"]} name={value["name"]} '
                  f'sequence_bytes={value["sequence_bytes"]} '
                  f'collection_bytes={value["collection_bytes"]}', file=sys.stderr, flush=True)
        try:
            records, size = prepare(kind, source_path, text, names, progress)
        except (OSError, RuntimeError) as error:
            record(dict(stage=stage, returncode=1, error=str(error)))
            raise
        record(dict(stage=stage, returncode=0, records=records, collection_bytes=size,
                    wall_seconds=time.monotonic()-begin,
                    peak_rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss))
    if a.agc:
        text = work / 'collection.txt'
        # The committed streamer emits the names sidecar beside this -o path.
        run('prepare-agc', [agc, source_path, '--revlines', '--upper', '--sep', '1e', '-o', text])
        names = work / 'collection.txt.names.tsv'
    with text.open('rb') as f:
        # O(1) terminal-byte lookup for padding normalization; never scan or
        # rewrite raw text. The caller owns the reserved-separator contract.
        if not text.stat().st_size:
            raise RuntimeError('empty input text')
        f.seek(-1, 2)
        terminal = f.read(1)[0]
    prefix = work / 'parse'
    run('parse', [pfp, '-t', text, '-o', prefix, '-w', 10, '-p', 100, '-j', a.threads, '--tmp-dir', work])
    run('parse-l2', [pfp, '-i', str(prefix)+'.parse', '-w', 5, '-p', 11, '-j', a.threads, '--tmp-dir', work])
    # Upstream's rdbuf merge sets failbit on an empty chunk. Tiny parses can
    # create such chunks; one chunk avoids this without changing .ssa code.
    chunks = 1 if text.stat().st_size < 1_000_000 else 50
    run('rpfbwt', [rpf, '--l1-prefix', prefix, '--w1', 10, '--w2', 5, '--threads', a.threads, '--chunks', chunks, '--tmp-dir', work])
    ri4, heads, agg, chi = [work / ('fresh.' + ext) for ext in ('ri4', 'head_sa', 'agg', 'sA')]
    run('endpoints', [tools/'rpfbwt_endpoints', prefix, ri4, heads, f'{terminal:02x}'])
    if a.expect_heads:
        run('gate-heads', ['/usr/bin/cmp', heads, pathlib.Path(a.expect_heads).absolute()])
    if a.expect_ri4:
        run('gate-runs-tails', ['/usr/bin/cmp', ri4, pathlib.Path(a.expect_ri4).absolute()])
    with ri4.open('rb') as header:
        _, _, strings, _ = struct.unpack('<4Q', header.read(32))
    if strings > 1 and not (a.expect_heads and a.expect_ri4):
        raise RuntimeError('multi-string PFP/BCR collection ordering is unvalidated; explicit endpoint byte gates are required')
    run('slim', [tools/'slim_dump', '--slim', '--resolve-ri4', '--dict-stream', '--ri4', ri4,
                 '--head-sa', heads, '--parse', prefix, '-t', a.threads, '-o', agg])
    sweep = run('sweep', [a.xsa, 'chi-rspace', '--stream-agg', '--ri4', ri4, '--agg', agg, '-o', chi])
    count = chi.stat().st_size // 8
    reported = re.search(r'chi = (\d+)', sweep.read_text())
    if chi.stat().st_size % 8 or not reported or int(reported[1]) != count:
        raise RuntimeError('internal chi count/file gate mismatch')
    if a.expect_chi is not None and count != a.expect_chi:
        raise RuntimeError(f'chi gate mismatch: {count} != {a.expect_chi}')
    if a.verify_text_sample:
        run('verify-text-sample', [tools/'sxi_text_audit', text, ri4, agg, chi, a.verify_text_sample])
    # Writer validates range, uniqueness, endpoint consistency and checksums.
    # Write on the destination filesystem, validate, then publish no-clobber.
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.xsa-publish-', dir=out.parent) as publish:
        candidate = pathlib.Path(publish)/'candidate.sxi'
        cmd = [tools/'sxi_write', '--ri4', ri4, '--heads', heads, '--chi', chi, '--output', candidate]
        cmd += ['--mode', (a.mode if a.mode != 'auto' else ('text' if a.text else 'dna')), '--orientation', 'reversed' if a.agc else 'forward']
        if names:
            cmd += ['--names', names]
        run('write', cmd)
        run('validate', [a.xsa, 'sxi-info', candidate])
        os.link(candidate, out)
    record(dict(status='PASS', output=str(out), chi=count, work=str(work)))
    print(json.dumps(dict(sxi=str(out), chi=count, work=str(work), log=str(journal))))

if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        sys.exit('xsa build: ' + str(error) + '; no final SXI published')
