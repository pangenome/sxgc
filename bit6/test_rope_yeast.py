#!/usr/bin/env python3
"""Read-only yeast235 oracle: native byte parity and SMEM TSV counts/positions."""
import argparse
import collections
import json
import pathlib
import resource
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--work', default='bit6/sxi_logs/rope-compat/yeast')
p.add_argument('--index', default='/tmp/sxi-query-yeast235.sxi')
p.add_argument('--scratch', default='/tmp/sxi-repair2/yeast235/xsa-build-26al38ee')
p.add_argument('--xsa', default='xsa/target/release/xsa')
p.add_argument('--baseline', default='/tmp/sxi-rope-native-baseline')
p.add_argument('--brute', default='/tmp/sxi-query-brute')
a = p.parse_args()
resource.setrlimit(resource.RLIMIT_AS, (9 << 30, 9 << 30))
work, scratch = pathlib.Path(a.work), pathlib.Path(a.scratch)
work.mkdir(parents=True, exist_ok=True)
commands = (work / 'commands.jsonl').open('w')

def run(args, label):
    args = list(map(str, args))
    result = subprocess.run(['/usr/bin/time', '-v', '-o', str(work / (label + '.time')), *args], capture_output=True)
    (work / (label + '.stdout')).write_bytes(result.stdout)
    (work / (label + '.stderr')).write_bytes(result.stderr)
    commands.write(json.dumps(dict(command=args, exit=result.returncode)) + '\n')
    commands.flush()
    assert result.returncode == 0, result.stderr.decode()
    return result.stdout

names = [line.split('\t') for line in (scratch / 'collection.txt.names.tsv').read_text().splitlines()]
n = (scratch / 'collection.txt').stat().st_size
reads = []
with (scratch / 'collection.txt').open('rb') as f:
    for i, doc in enumerate([12, 555]):
        name, fs, length = names[doc]
        start = n - 1 - int(fs) - int(length)
        f.seek(start + 123)
        seq = f.read(45)[::-1]
        if i == 1:
            seq = seq[:23] + (b'A' if seq[23:24] != b'A' else b'C') + seq[24:]
        reads.append(('sample' + str(i), seq))
fa = work / 'reads.fa'
fa.write_bytes(b''.join(b'>' + name.encode() + b'\n' + seq + b'\n' for name, seq in reads))
rc = bytes.maketrans(b'ACGTN', b'TGCAN')
br = work / 'oriented.tsv'
br.write_bytes(b''.join(name.encode() + b'\t' + strand + b'\t' + q + b'\n' for name, seq in reads for strand, q in [(b'+', seq[::-1]), (b'-', seq.translate(rc))]))
raw = run([a.brute, scratch / 'collection.txt', br, scratch / 'collection.txt.names.tsv', 20], 'brute')
want = []
for row in raw.decode().splitlines():
    name, doc, off, length, start, strand = row.split('\t')
    doc = int(doc)
    want.append(dict(read=name, doc_id=doc, name=names[doc][0], offset=int(off), len=int(length), qstart=int(start), strand=strand))
base = [a.xsa, 'mems', '--sxi', a.index, '--reads', fa, '--min-len', 20, '-j', 2]
native = run(base + ['--out', 'native'], 'native')
canon = lambda rows: sorted(json.dumps(x, sort_keys=True) for x in rows)
assert canon(want) == canon([json.loads(x) for x in native.splitlines()])
assert native == run([a.baseline, *base[1:]], 'baseline')
all_mems = collections.defaultdict(list)
for hit in want:
    all_mems[hit['read'], hit['qstart'], hit['qstart'] + hit['len']].append(f"{hit['name'].split()[0]}:{hit['strand']}:{hit['offset']}")
smems = {key: hits for key, hits in all_mems.items() if not any(name == key[0] and start <= key[1] and end >= key[2] and (start, end) != key[1:] for name, start, end in all_mems)}

def check(raw, expected, positions):
    actual = {}
    for line in raw.decode().splitlines():
        name, start, end, count, *pos = line.split('\t')
        key = (name, int(start), int(end))
        assert key not in actual
        actual[key] = int(count), collections.Counter(pos)
    assert actual == {key: (len(pos), collections.Counter(pos) if positions else collections.Counter()) for key, pos in expected.items()}

rope = base + ['--out', 'ropebwt3']
check(run(rope, 'rope-counts'), smems, False)
check(run(rope + ['-p', 5000], 'rope-positions'), smems, True)
check(run(rope + ['--mem', '-p', 5000], 'rope-all-mems'), all_mems, True)
print(json.dumps(dict(status='PASS', reads=len(reads), native_mems=len(want), smems=len(smems), smem_occurrences=sum(map(len, smems.values())), checks=['direct-text oracle', 'native baseline byte parity', 'SMEM containment', 'authoritative counts', 'all forward positions', 'all-MEM aggregation'])))
