"""Deterministic query probes from retained gate reads; no corpus access."""
import json
import pathlib
import random

root = pathlib.Path(__file__).parent
gates = root.parent / 'sxi2-v6-2'
reads_dir = root.parent / 'sxi2-v5'
names = {'yeast235': 'yeast235', 'pile-frag': 'pile-frag', 'k10': 'k10-retry'}
for corpus, gate_name in names.items():
    seqs = []
    for path in sorted(reads_dir.glob(corpus + '*.fa')):
        seqs.extend(line.rstrip('\n') for line in path.read_text().splitlines()
                    if line and not line.startswith('>'))
    gate = json.loads((gates / f'{gate_name}-http-gate.json').read_text())
    rng = random.Random(7031)
    out = [f'gate\t{gate["pattern"]}']
    for i in range(1000):
        seq = rng.choice(seqs)
        length = rng.randint(12, min(40, len(seq)))
        start = rng.randrange(len(seq) - length + 1)
        pattern = seq[start:start + length]
        # Each fourth probe tests a near-match extension, common in MEM scans.
        if i % 4 == 0:
            j = rng.randrange(len(pattern))
            alphabet = 'ACGT' if corpus != 'pile-frag' else 'aetinsor '
            pattern = pattern[:j] + rng.choice(alphabet) + pattern[j + 1:]
        out.append(f'probe{i:04d}\t{pattern}')
    (root / f'{corpus}.queries.tsv').write_text('\n'.join(out) + '\n')
