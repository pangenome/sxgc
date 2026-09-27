#!/usr/bin/env python3
"""Reserved-separator acceptance gates, including an independent tiny cyclic oracle.

The quadratic oracle is test-only, bounded to 2003 bytes. A successful text
control alone cannot certify cyclic suffix order. Failures are retained.
"""
import argparse
import hashlib
import json
import pathlib
import random
import struct
import subprocess

from test_sxi_input_modes import members, parser_contracts
from sxi_sequence_input import CONTRACT


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', required=True)
    p.add_argument('--xsa', required=True)
    p.add_argument('--agc2flat', required=True)
    p.add_argument('--reserved-agc', required=True)
    p.add_argument('--log-dir', required=True)
    a = p.parse_args()
    work, logs = pathlib.Path(a.work), pathlib.Path(a.log_dir)
    work.mkdir(parents=True, exist_ok=True)
    logs.mkdir(parents=True, exist_ok=True)
    parser_contracts(work)
    summary = {'checks': [], 'blockers': []}

    def record(name, passed, **details):
        row = dict(check=name, passed=passed, **details)
        summary['checks'].append(row)
        print(json.dumps(row), flush=True)
        if not passed:
            summary['blockers'].append(name)

    def build(label, kind, source, extra=(), rejection=False):
        out = work / (label + '.sxi')
        command = [a.xsa, 'build', '--' + kind, str(source), '-o', str(out),
                   '--threads', '2', '--scratch', str(work), '--log-dir', str(logs),
                   *map(str, extra)]
        result = subprocess.run(command, capture_output=True, text=True)
        (logs / (label + '.log')).write_text(result.stderr + result.stdout)
        line = next(line for line in result.stderr.splitlines() if line.startswith('xsa build: work='))
        scratch = pathlib.Path(line.split('work=', 1)[1].split(' log=', 1)[0])
        if rejection:
            evidence = result.stderr + ''.join(path.read_text() for path in logs.glob(label + '-' + scratch.name + '.prepare-*.log'))
            record(label, result.returncode != 0 and CONTRACT in evidence and not out.exists()
                   and not (scratch / 'parse.parse').exists(), command=command)
        else:
            record(label + '-publication', result.returncode == 0 and out.exists(),
                   command=command, stderr=result.stderr)
        return out, scratch

    rng = random.Random(30)
    records = [bytes(rng.choice(b'ACGT') for _ in range(n)) for n in (666, 667, 667)]
    text = b'\x1e'.join(records) + b'\x1e'
    source = work / 'control.txt'
    source.write_bytes(text)
    baseline, baseline_work = build('sep-text', 'text', source)
    record('raw-text-as-is', source.read_bytes() == text and not (baseline_work / 'collection.txt').exists())
    gates = ['--expect-heads', baseline_work / 'fresh.head_sa', '--expect-ri4', baseline_work / 'fresh.ri4']
    for kind in ('fasta', 'fastq'):
        path = work / kind
        raw = b''
        for i, seq in enumerate(records):
            name = str(i).encode()
            if kind == 'fasta':
                raw += b'>' + name + b'\n' + seq[:70] + b'\n' + seq[70:] + b'\n'
            else:
                raw += b'@' + name + b'\n' + seq + b'\n+\n' + b'I' * len(seq) + b'\n'
        path.write_bytes(raw)
        output, scratch = build('sep-' + kind, kind, path, gates)
        record(kind + '-materialized-bytes', (scratch / 'collection.txt').read_bytes() == text)
        record(kind + '-endpoint-byte-gates', all((scratch / name).read_bytes() ==
               (baseline_work / name).read_bytes() for name in ('fresh.ri4', 'fresh.head_sa')))
        record(kind + '-core-members', output.exists() and baseline.exists() and
               all(members(output)[i] == members(baseline)[i] for i in range(1, 6)))

    # Prove the unchanged tap matches its padded producer text first.
    padded = text + b'\x02' * 10
    padded_sa = sorted(range(len(padded)), key=lambda i: padded[i:] + padded[:i])
    padded_bwt = bytes(padded[(i-1) % len(padded)] for i in padded_sa)
    for suffix, boundary in [('.ssa', lambda i: not i or padded_bwt[i] != padded_bwt[i-1]),
                             ('.ssa_t', lambda i: i+1 == len(padded) or padded_bwt[i] != padded_bwt[i+1])]:
        expected = [padded_sa[i] for i in range(len(padded)) if boundary(i)]
        raw = (baseline_work / ('parse' + suffix)).read_bytes()
        actual = list(struct.unpack(f'<{len(raw)//8}Q', raw))
        record('producer-padded-oracle' + suffix, actual == [len(expected), *expected])

    # Compare normalized endpoint data against actual cyclic rotations of T.
    sa = sorted(range(len(text)), key=lambda i: text[i:] + text[:i])
    bwt = bytes(text[(i-1) % len(text)] for i in sa)
    heads = [sa[i] for i in range(len(text)) if not i or bwt[i] != bwt[i-1]]
    ri4 = (baseline_work / 'fresh.ri4').read_bytes()
    n, k, runs = struct.unpack_from('<3Q', ri4, 8)
    chars = ri4[2080:2080+runs]
    lengths = struct.unpack_from(f'<{runs}I', ri4, 2080+runs)
    actual_bwt = b''.join(bytes([c]) * length for c, length in zip(chars, lengths))
    actual_heads = list(struct.unpack(f'<{runs}Q', (baseline_work / 'fresh.head_sa').read_bytes()))
    record('independent-cyclic-order', actual_bwt == bwt and actual_heads == heads,
           n=n, k=k, actual_runs=runs, cyclic_runs=len(heads),
           first_actual_heads=actual_heads[:10], first_cyclic_heads=heads[:10],
           bwt_equal=actual_bwt == bwt, heads_equal=actual_heads == heads,
           text_sha256=hashlib.sha256(text).hexdigest())

    for kind, raw in [('fasta', b'>a\nAC\x1eGT\n'),
                      ('fastq', b'@a\nAC\x1eGT\n+\nIIIII\n')]:
        path = work / ('reserved.' + kind)
        path.write_bytes(raw)
        build('reserved-' + kind, kind, path, rejection=True)
    build('reserved-agc', 'agc', pathlib.Path(a.reserved_agc), rejection=True)

    # Isolate the separator change from stream order and coordinate semantics.
    fixture = pathlib.Path('/tmp/laneV/agc-test/tiny.agc')
    agc_output, agc_work = build('sep-agc', 'agc', fixture)
    control_output, control_work = build('sep-agc-text', 'text', agc_work / 'collection.txt',
        ['--expect-heads', agc_work / 'fresh.head_sa', '--expect-ri4', agc_work / 'fresh.ri4'])
    record('agc-endpoint-byte-gates', all((agc_work / name).read_bytes() ==
           (control_work / name).read_bytes() for name in ('fresh.ri4', 'fresh.head_sa')))
    record('agc-core-members', agc_output.exists() and control_output.exists() and
           all(members(agc_output)[i] == members(control_output)[i] for i in range(1, 6)))
    for label, flags in [('flat', []), ('revlines', ['--revlines']), ('reverse', ['--reverse'])]:
        outputs = []
        for suffix, sep in [('default', []), ('newline', ['--sep', '0a'])]:
            output = work / (label + '-' + suffix + '.txt')
            result = subprocess.run([a.agc2flat, str(fixture), *flags, *sep, '-o', str(output)],
                                    capture_output=True, text=True)
            assert result.returncode == 0, result.stderr
            outputs.append(output)
        first, second = outputs
        record('agc-' + label + '-separator-and-offsets',
               first.read_bytes() == second.read_bytes().replace(b'\n', b'\x1e') and
               pathlib.Path(str(first) + '.names.tsv').read_bytes() ==
               pathlib.Path(str(second) + '.names.tsv').read_bytes())
    (logs / 'separator-gates.json').write_text(json.dumps(summary, indent=2) + '\n')
    return bool(summary['blockers'])


if __name__ == '__main__':
    raise SystemExit(main())
