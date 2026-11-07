#!/usr/bin/env python3
"""P2: check quantitative TeX claims against the referee ledger (no dependencies)."""
from pathlib import Path
import re

root = Path(__file__).resolve().parent
tex = (root / 'main.tex').read_text()
ledger = (root / 'NUMBERS.md').read_text()
rows = {}
for line in ledger.splitlines():
    m = re.match(r'\| ([a-z]+) \| `([^`]+)` \| `([^`]+)` \| (.+) \| ([A-Z]) \|$', line)
    if m:
        assert m[1] not in rows, f'duplicate ledger ID: {m[1]}'
        rows[m[1]] = m.groups()[1:]
seen = {}
occurrences = 0
for m in re.finditer(r'\\qnum\{([a-z]+)\}\{', tex):
    start = i = m.end()
    depth = 1
    while depth:
        depth += (tex[i] == '{') - (tex[i] == '}')
        i += 1
    value = tex[start:i-1]
    assert m[1] in rows, f'missing ledger entry: {m[1]}'
    assert value == rows[m[1]][0], f'value differs: {m[1]}'
    assert m[1] not in seen or seen[m[1]] == value, f'inconsistent value: {m[1]}'
    seen[m[1]] = value
    occurrences += 1
assert set(rows) == set(seen), 'unused ledger rows'
assert '| build-threads | 16 | RESUME.md' in ledger
assert '| build-memory | 24 | RESUME.md' in ledger
assert '--threads 16 --memory-gb 24' in tex
print(f'P2 PASS: {len(rows)} sourced claim IDs, {occurrences} occurrences; '
      f'2 documented CLI parameters; {len(rows)+2} ledger quantities total; '
      '0 missing or inconsistent values.')
print('P2 scope: structural conventions and non-empirical identifiers are listed '
      'separately; source truth and command limitations require manual review.')
