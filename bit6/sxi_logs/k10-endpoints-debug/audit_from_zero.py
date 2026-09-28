import hashlib
import json
import pathlib
import struct
import subprocess

LOGS = pathlib.Path(__file__).resolve().parent
ROOT = LOGS.parents[2]


def read_json(name):
    return json.loads((LOGS / name).read_text())


def save(name, value):
    (LOGS / name).write_text(json.dumps(value, indent=2) + '\n')


def members(path):
    path = pathlib.Path(path)
    with path.open('rb') as f:
        h = f.read(64)
        assert h[:4] == b'SXI1'
        n, k, r = struct.unpack_from('<3Q', h, 8)
        count = struct.unpack_from('<I', h, 32)[0]
        size = struct.unpack_from('<Q', h, 40)[0]
        assert size == path.stat().st_size
        result = []
        for _ in range(count):
            member, codec, offset, size, number, crc, reserved = struct.unpack('<IIQQQII', f.read(40))
            assert not reserved
            result.append(dict(id=member, codec=codec, offset=offset, bytes=size, count=number, crc32=crc))
    return dict(path=str(path), bytes=path.stat().st_size, n=n, k=k, r=r, members=result)


manifest = read_json('from-zero-manifest.json')
assert manifest['status'] == 'PASS' and len(manifest['runs']) == 2
builds = []
for run in manifest['runs']:
    assert run['returncode'] == 0
    output = pathlib.Path(run['output'])
    work = pathlib.Path(run['work'])
    journal = LOGS / (output.stem + '-' + work.name + '.jsonl')
    rows = [json.loads(line) for line in journal.read_text().splitlines()]
    assert rows[-1]['status'] == 'PASS'
    stages = [row for row in rows if 'returncode' in row]
    assert all(row['returncode'] == 0 and row['peak_rss_kib'] * 1024 < 150_000_000_000 for row in stages)
    required = {'parse', 'parse-l2', 'rpfbwt', 'endpoints', 'slim', 'sweep', 'write', 'validate'}
    if run['label'] == 'B':
        required |= {'gate-heads', 'gate-runs-tails'}
    assert {row['stage'] for row in stages} == required
    commands = {row['stage']: row['command'] for row in rows if 'command' in row}
    assert all('/retained/' not in arg for command in commands.values() for arg in command)
    for stage in ('parse', 'parse-l2'):
        assert commands[stage][commands[stage].index('-j') + 1] == '16'
    assert commands['rpfbwt'][commands['rpfbwt'].index('--threads') + 1] == '16'
    assert commands['endpoints'][1] == str(work / 'parse') and commands['endpoints'][-1] == '1e'
    if run['label'] == 'B':
        control = pathlib.Path(manifest['runs'][0]['work'])
        assert commands['gate-heads'][-1] == str(control / 'fresh.head_sa')
        assert commands['gate-runs-tails'][-1] == str(control / 'fresh.ri4')
        for name in ('parse.parse', 'parse.dict', 'parse.rlebwt', 'parse.ssa', 'parse.ssa_t'):
            assert not (work / name).samefile(control / name)
    frame = members(output)
    assert (frame['n'], frame['k']) == (30_151_407_545, 1)
    chi = next(member['count'] for member in frame['members'] if member['id'] == 5)
    assert chi == rows[-1]['chi']
    builds.append(dict(label=run['label'], journal=str(journal), stages=stages, frame=frame, chi=chi))

assert builds[0]['chi'] == builds[1]['chi']
subprocess.run(['/usr/bin/time', '-v', '-o', str(LOGS / 'publication-cmp.time'),
                'cmp', manifest['runs'][0]['output'], manifest['runs'][1]['output']], check=True)
save('from-zero-stages.json', builds)
save('member-sizes.json', builds[1]['frame'])
chi = builds[1]['chi']
delta = chi - 1_627_063_183
save('boundary-delta.json', dict(canonical_chi=chi, historical_bcr_chi=1_627_063_183,
                               delta=delta, identity=f'{chi} = 1627063183 + ({delta})',
                               interpretation='Measured cardinality difference across conventions. This arithmetic identity is not a same-frame BCR witness-set equality or a proof that only separator-boundary witnesses changed.'))

retained = read_json('retained-input-stats.json')
assert all(pathlib.Path(p).stat().st_size == v['size'] and pathlib.Path(p).stat().st_mtime_ns == v['mtime_ns'] for p, v in retained.items())
source = read_json('fresh-input.json')
assert pathlib.Path(source['path']).stat().st_size == source['size']
assert pathlib.Path(source['path']).stat().st_mtime_ns == source['mtime_ns']
for name, expected in read_json('component-hashes.json').items():
    path = pathlib.Path(name)
    if not path.is_absolute():
        path = ROOT / path
    assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, name
staged = subprocess.check_output(['git', 'diff', '--cached', '--name-only'], cwd=ROOT, text=True).strip()
assert not staged
subprocess.run(['git', 'diff', '--check'], cwd=ROOT, check=True)
peak = 0
with (LOGS / 'from-zero-rss.jsonl').open() as f:
    for line in f:
        peak = max(peak, json.loads(line)['total_rss_kib'])
assert peak * 1024 < 150_000_000_000
save('final-hygiene.json', dict(no_staged_files=True, retained_inputs_unchanged=True,
                                source_unchanged=True, component_hashes_unchanged=True,
                                from_zero_peak_rss_kib=peak, publications_byte_identical=True,
                                independent_review='pending'))
gate = read_json('gate-summary.json')
gate.update(from_zero='PASS', published=builds[1]['frame']['path'], canonical_chi=chi,
            historical_bcr_delta=delta, publications_byte_identical=True)
save('gate-summary.json', gate)
(LOGS / 'ACCEPTANCE.md').write_text(f'''# K10 endpoint fix and from-zero acceptance

Published: `{builds[1]['frame']['path']}`.
Canonical measured chi: **{chi:,}**.
Boundary-delta identity: **{chi:,} = 1,627,063,183 + ({delta:+,})**.
This is a measured cardinality difference across conventions, not an independently
proved BCR witness-set identity or an attribution of every difference to separators.

Run A and run B each performed all parse, producer, endpoint, slim, sweep,
write, and validation stages from empty, separate scratch directories with
16 threads. Run B used only A's fresh endpoints as byte gates. Both complete
SXI publications are byte-identical. Stage commands, timings, and RSS are in
the journals and `from-zero-stages.json`; member sizes are in `member-sizes.json`.
Sampled fresh-build process-tree peak RSS: {peak * 1024 / 1e9:.3f} GB.

The retained k10 completion was exclusively a debug gate: 7 seam classes,
1,161 rows, 8 discovery steps; n=30,151,407,545, r=1,859,825,862. Its ri4 has
17,435,869,551 bytes and heads have 14,878,606,896 bytes. No retained k10
slim, sweep, or write stage was run. Originals retain their sizes and mtimes.
The debug process peaked at 130,763,968 KiB; sampled combined endpoint
regression jobs peaked at 141.194 GB, below 150 GB.

Root cause: the failed journal omitted TERMINAL_HEX and used a stale binary.
Live instrumentation localized the check to row 0, 0x1e versus default 0x0a.
The adapter now validates and infers the padding-row terminal for legacy
callers and shares clean/straddled padding checks. No tap, producer, pipeline,
front-end, seam-repair, slim, sweep, or writer change was made by this task.

Validation: 7 new padding checks, 2,214 cyclic endpoint cases, 74 dense
aggregate/witness checks, 47 separator checks, and exact yeast/yeast235
endpoint regressions pass. The first separator attempt used a stale agc2flat
without --sep; rerunning with the existing separator-capable binary passed.
No staging or commits; git diff --check passes. Prior uncommitted lane work
is preserved. See fix.diff, diagnosis.md, REVIEW.md, and final-hygiene.json.
Both runs pin XSA_TOOLS=/tmp/k10-endpoints-debug/tools and
XSA_PIPELINE=/tmp/sxgc-laneV/bit6/sxi_pipeline.py. The legacy
/tmp/laneV/tools installation was not overwritten; see build-environment.json.

The required independent reviewer gate remains pending. No supervisor tool
was available, and no external approval is claimed.
''')
print(json.dumps(dict(status='PASS', publication=builds[1]['frame'], chi=chi,
                      historical_delta=delta, no_staged_files=True, external_review='pending')))
