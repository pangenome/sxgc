#!/usr/bin/env python3
"""Real ropebwt3 parity plus independent occurrence/SMEM oracle (tiny fixtures).
All commands and outputs are retained; child processes have a 9 GiB AS limit.
"""
import argparse
import collections
import gzip
import json
import pathlib
import random
import resource
import struct
import subprocess
from test_sxi_separator import cyclic_frame

P = argparse.ArgumentParser(description=__doc__)
P.add_argument('--work', required=True)
P.add_argument('--rope', required=True)
P.add_argument('--xsa', default='xsa/target/release/xsa')
P.add_argument('--baseline', default='/tmp/sxi-rope-native-baseline')
P.add_argument('--writer', default='/tmp/sxi-query-writer')
P.add_argument('--battery', action='store_true', help='also relabel the seven retained construction fixtures to DNA in O(r)')
A = P.parse_args()
resource.setrlimit(resource.RLIMIT_AS, (9 << 30, 9 << 30))
work = pathlib.Path(A.work).absolute()
work.mkdir(parents=True, exist_ok=True)
commands = (work / 'commands.jsonl').open('w')
RC = bytes.maketrans(b'ACGTRYSWKMBDHVNacgtryswkmbdhvn', b'TGCAYRSWMKVHDBNtgcayrswmkvhdbn')

def run(args, label, ok=True):
    args = list(map(str, args))
    p = subprocess.run(['/usr/bin/time', '-v', '-o', str(work / (label + '.time')), *args], capture_output=True)
    (work / (label + '.stdout')).write_bytes(p.stdout)
    (work / (label + '.stderr')).write_bytes(p.stderr)
    commands.write(json.dumps(dict(command=args, exit=p.returncode)) + '\n')
    commands.flush()
    assert (p.returncode == 0) == ok, (args, p.returncode, p.stderr.decode())
    return p.stdout

def fasta(path, records):
    path.write_bytes(b''.join(b'>' + name.encode() + b'\n' + seq + b'\n' for name, seq in records))

def fixture(label, records, reverse=False, dna=True):
    text = b'\x1e'.join(seq[::-1] if reverse else seq for _, seq in records) + b'\x1e'
    sa, bwt, runs = cyclic_frame(text)
    n, r = len(text), len(runs)
    ri, head, names, index = [work / (label + ext) for ext in ('.ri4', '.heads', '.names', '.sxi')]
    counts = [bwt.count(c) for c in range(256)]
    C, acc = [], 0
    for c in counts:
        C.append(acc)
        acc += c
    width = (n - 1).bit_length() or 1
    packed = sum((n - 1 - v[3]) << (i * width) for i, v in enumerate(runs))
    ri.write_bytes(struct.pack('<IIQQQ256Q', 0x52585349, 4, n, 1, r, *C) + bytes(v[0] for v in runs) + struct.pack(f'<{r}I', *(v[1] for v in runs)) + struct.pack('<QB', r * width, width) + packed.to_bytes(((r * width + 63) // 64) * 8, 'little'))
    head.write_bytes(struct.pack(f'<{r}Q', *(v[2] for v in runs)))
    rows, off = [], 0
    for name, seq in records:
        rows.append(f'{name}\t{n - 1 - off - len(seq)}\t{len(seq)}\n')
        off += len(seq) + 1
    names.write_text(''.join(rows))
    index.unlink(missing_ok=True)
    run([A.writer, '--ri4', ri, '--heads', head, '--names', names, '--mode', 'dna' if dna else 'text', '--orientation', 'reversed' if reverse else 'forward', '--output', index], label + '-writer')
    return index

def brute(records, reads, minimum, dna=True):
    """Direct text extension per occurrence, independent of MS/FM intervals."""
    result = collections.defaultdict(list)
    for name, q in reads:
        name = name.split()[0]
        for strand, seq in ([('+', q), ('-', q.translate(RC)[::-1])] if dna else [('+', q)]):
            for record, text in records:
                for i in range(len(seq) - minimum + 1):
                    pos = text.find(seq[i:i + minimum])
                    while pos >= 0:
                        if not (i and pos and seq[i - 1] == text[pos - 1]):
                            length = minimum
                            while i + length < len(seq) and pos + length < len(text) and seq[i + length] == text[pos + length]:
                                length += 1
                            start = i if strand == '+' else len(seq) - i - length
                            result[name, start, start + length].append(f'{record.split()[0]}:{strand}:{pos}')
                        pos = text.find(seq[i:i + minimum], pos + 1)
    return dict(result)

def smems(all_mems):
    return {key: hits for key, hits in all_mems.items() if not any(name == key[0] and start <= key[1] and end >= key[2] and (start, end) != key[1:] for name, start, end in all_mems)}

def table(raw, sampled_count=False):
    result = {}
    for line in raw.decode().splitlines():
        cols = line.split('\t')
        key = (cols[0], int(cols[1]), int(cols[2]))
        assert key not in result, ('duplicate interval', key)
        positions = cols[4:]
        if sampled_count and positions:
            assert int(positions[0]) == len(positions) - 1
            positions = positions[1:]
        result[key] = (int(cols[3]), collections.Counter(positions))
    return result

def check(label, records, reads, minimum, reverse=False, dna=True, real=True, index=None):
    index = index or fixture(label, records, reverse, dna)
    fa, ref, fmd = [work / (label + ext) for ext in ('.reads.fa', '.refs.fa', '.fmd')]
    fasta(fa, reads)
    fasta(ref, records)
    base = [A.xsa, 'mems', '--sxi', index, '--reads', fa, '--min-len', minimum]
    native = run(base, label + '-native')
    assert native == run([A.baseline, *base[1:]], label + '-baseline')
    assert native == run(base + ['--out', 'native', '--mem'], label + '-explicit-native')
    base += ['--out', 'ropebwt3']
    all_mems = brute(records, reads, minimum, dna)
    expected = smems(all_mems)
    counts = {key: (len(pos), collections.Counter()) for key, pos in expected.items()}
    full = {key: (len(pos), collections.Counter(pos)) for key, pos in expected.items()}
    actual = run(base, label + '-counts')
    assert table(actual) == counts, (label, table(actual), counts)
    assert actual == run(base + ['-j', 3], label + '-threads')
    assert table(run(base + ['-p', 100000], label + '-positions')) == full, label
    assert table(run(base + ['--mem', '-p', 100000], label + '-all-mems')) == {key: (len(pos), collections.Counter(pos)) for key, pos in all_mems.items()}
    sampled = table(run(base + ['-p', 2], label + '-cap'))
    assert sampled.keys() == counts.keys()
    for key, (count, positions) in sampled.items():
        assert count == counts[key][0] and sum(positions.values()) == min(2, count)
        assert positions <= full[key][1]
    cov, gaps = [], []
    for name, seq in reads:
        name = name.split()[0]
        covered = [any(n == name and s <= i < e for n, s, e in expected) for i in range(len(seq))]
        if any(covered):
            cov.append(f'{name}\t{len(seq)}\t{sum(covered)}\n')
        start = 0
        while start < len(seq):
            if covered[start]:
                start += 1
                continue
            end = start + 1
            while end < len(seq) and not covered[end]:
                end += 1
            if end - start >= 2:
                gaps.append(f'{name}\t{start}\t{end}\t{len(seq)}\t' + seq[start:end].decode() + '\n')
            start = end
    actual_cov = run(base + ['--cov'], label + '-cov')
    actual_gap = run(base + ['--gap', 2, '--gap-seq', '--cov', '-p', 2], label + '-gap')
    gap_no_seq = b''.join(b'\t'.join(line.split(b'\t')[:4]) + b'\n' for line in actual_gap.splitlines())
    assert run(base + ['--gap', 2], label + '-gap-no-seq') == gap_no_seq
    assert table(run(base + ['--positions', 0], label + '-zero-cap')) == counts
    assert run(base + ['--mem', '--cov'], label + '-all-cov') == actual_cov
    assert actual_cov == ''.join(cov).encode()
    assert actual_gap == ''.join(gaps).encode()
    if real:
        run([A.rope, 'build', '-t1', '-do', fmd, ref], label + '-rope-build')
        run([A.rope, 'ssa', '-t1', '-s3', '-o', str(fmd) + '.ssa', fmd], label + '-rope-ssa')
        with gzip.open(str(fmd) + '.len.gz', 'wb') as f:
            f.write(''.join(f'{name.split()[0]}\t{len(seq)}\n' for name, seq in records).encode())
        rb = [A.rope, 'mem', '-t1', '-l', minimum, fmd, fa]
        assert table(run(rb, label + '-rope-counts')) == counts, label
        assert table(run(rb[:2] + ['-p', 100000] + rb[2:], label + '-rope-positions'), True) == full, label
        assert run(rb[:2] + ['--cov'] + rb[2:], label + '-rope-cov') == actual_cov
        assert run(rb[:2] + ['--gap', 2, '--gap-seq'] + rb[2:], label + '-rope-gap') == actual_gap
    print(json.dumps(dict(test=label, reads=len(reads), smems=len(expected), maximal_occurrences=sum(map(len, all_mems.values())), rope_parity=real, status='PASS')), flush=True)
    return base

rng = random.Random(466)
records = [('a', b'ACGACGTACG'), ('b', b'ACGTTTACG'), ('c', b'TTACGAC'), ('pal', b'ATATAT')]
reads = [('r' + str(i), bytes(rng.choice(b'ACGTN') for _ in range(rng.randrange(1, 18)))) for i in range(50)] + [('exact', records[0][1]), ('repeat', b'ACGACG'), ('left-shorter', b'TTACGACGT')]
base = check('dna', records, reads, 2)
check('reversed', records, reads, 2, reverse=True)
check('periodic', [(str(i), b'ACG' * 25) for i in range(7)], [('q', b'ACG' * 5)], 3)
for i in range(12):
    rs = [(str(j), bytes(rng.choice(b'ACGT') for _ in range(rng.randrange(3, 32)))) for j in range(3)]
    qs = [(str(j), bytes(rng.choice(b'ACGT') for _ in range(15))) for j in range(5)]
    check('random' + str(i), rs, qs, 1)
check('boundaries', [('first comment', b'GATTAC'), ('second', b'CGATTA'), ('duplicate', b'GATTAC')], [('boundary comment', b'ACCG'), ('minus', b'TAATCG'), ('missing', b'NNNN'), ('long', b'NNGATTACNN'), ('nested', b'CGATTAC')], 2)
check('text', [('doc x', b'abracadabra'), ('doc y', b'abra abrac')], [('r', b'abracadabracadabra')], 2, dna=False, real=False)
# Native alphabet is deliberately literal; this extension cannot be compared to
# ropebwt3's nt6 normalization (case-folding and ambiguous-base hard breaks).
check('literal', [('mixed', b'ACGTNacgtN')], [('q', b'ACGTNacgtN')], 1, real=False)
if A.battery:
    # Order-preserving alphabet relabeling leaves BWT order and all samples
    # unchanged. Reuse the certified core in O(r), without constructing SA/LCP
    # or scanning a text-length index structure. The original binary fixtures
    # are not DNA; both tested tools receive the SAME A/C/G/T relabeling.
    translation = bytes.maketrans(bytes([10, 11, 12, 13, 14]), b'\x1eACGT')
    for label in ['duplicates-600k', 'random-4-2k', 'random-4-20k', 'random-bin-20k', 'random-4-200k', 'satellite-18k', 'HOR-nested']:
        source = pathlib.Path('/tmp/laneV/g0-final', label + '.sxi').read_bytes()
        n, k, r = struct.unpack_from('<3Q', source, 8)
        num = struct.unpack_from('<I', source, 32)[0]
        members = {}
        for i in range(num):
            ident, codec, off, size, count, crc, reserved = struct.unpack_from('<IIQQQII', source, 64 + 40 * i)
            members[ident] = source[off:off + size]
        symbols = members[1][2048:2048 + r].translate(translation)
        lengths = struct.unpack(f'<{r}I', members[1][2048 + r:])
        counts = [0] * 256
        for symbol, length in zip(symbols, lengths):
            counts[symbol] += length
        C, acc = [], 0
        for count in counts:
            C.append(acc)
            acc += count
        prefix = work / ('battery-' + label)
        ri, head, anchors, names, index = [pathlib.Path(str(prefix) + ext) for ext in ('.ri4', '.heads', '.anc', '.names', '.sxi')]
        ri.write_bytes(struct.pack('<IIQQQ256Q', 0x52585349, 4, n, k, r, *C) + symbols + members[1][2048 + r:] + members[2])
        head.write_bytes(members[3])
        anchors.write_bytes(members[4])
        names.write_text(f'text\t0\t{n - 1}\n')
        index.unlink(missing_ok=True)
        run([A.writer, '--ri4', ri, '--heads', head, '--anchors', anchors, '--names', names, '--mode', 'dna', '--output', index], 'battery-' + label + '-writer')
        original = pathlib.Path('/tmp/laneY/bat', label + '.txt').read_bytes()
        assert len(original) == n and original[-1:] == b'\n' and b'\n' not in original[:-1]
        text = original[:-1].translate(translation)
        reads = [('sample' + str(i), text[pos:pos + 40]) for i, pos in enumerate([5, len(original) // 2, len(original) - 60])]
        check('battery-' + label, [('text', text)], reads, 12, index=index)
for flags in [['--out', 'bad'], ['--gap', '0'], ['--gap-seq'], ['--positions', '-1'], ['--sample', '2'], ['--ms']]:
    run(base + flags, 'invalid-' + flags[0][2:], False)
print('PASS real ropebwt3 count/SMEM/forward-position/coverage/gap parity, brute all-MEM/SMEM sets, native byte parity, capped positions, CLI errors')
