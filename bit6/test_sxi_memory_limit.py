#!/usr/bin/env python3
"""Build real tiny indexes with default, explicit, and inherited AS ceilings."""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--xsa', default='xsa/target/release/xsa')
args = parser.parse_args()
xsa = str(Path(args.xsa).resolve())

with tempfile.TemporaryDirectory(prefix='sxi-memory-limit-') as directory:
    root = Path(directory)
    source = root/'text'
    rng = random.Random(466)
    source.write_bytes(bytes(rng.choice(b'ACGT') for _ in range(2000)) + b'\x1e')
    outputs = []
    for label, options, inherited, expected in [
        ('default', [], None, None),
        ('large', ['--address-space-gb', '850'], None, 850_000_000_000),
        ('inherited', ['--address-space-gb', '850'], 1_000_000_000, 1_000_000_000),
    ]:
        output = root/(label+'.sxi')
        logs = root/label
        command = [xsa, 'build', '--text', str(source), '-o', str(output),
                   '--scratch', str(root), '--log-dir', str(logs), '--threads', '1', *options]
        if inherited:
            command = ['/usr/bin/prlimit', '--as='+str(inherited), *command]
        result = subprocess.run(command, capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        journal, = logs.glob('*.jsonl')
        events = [json.loads(line) for line in journal.read_text().splitlines()]
        assert events[0]['address_space_limit_bytes'] == expected, events[0]
        assert events[-1]['status'] == 'PASS', events[-1]
        outputs.append(output.read_bytes())
        print(json.dumps(dict(case=label, limit=expected, chi=events[-1]['chi'], status='PASS')), flush=True)
    assert outputs[0] == outputs[1] == outputs[2], 'ceiling changed index bytes'
    for value in ['-1', '9223372037', 'not-an-integer']:
        output = root/'invalid.sxi'
        result = subprocess.run([xsa, 'build', '--text', str(source), '-o', str(output),
                                 '--address-space-gb', value], capture_output=True, text=True)
        assert result.returncode != 0 and not output.exists(), (value, result)
    print('PASS: identical indexes; default/override/inherited hard ceiling; invalid limits rejected')
