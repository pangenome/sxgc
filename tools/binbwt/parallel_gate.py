#!/usr/bin/env python3
"""Exact parallelization gate using a Cargo-packaged bundle.

Emission must exit successfully, publish both artifacts, and journal DONE
before finish starts. Product and optional explicit-debug runs attest zero product dump scratch.
The checker expands published m1 against SA; debug also checks every band
stream endpoint. --product-only omits the additional debug-stream run.
"""
import argparse
import hashlib
import json
import mmap
import random
import struct
import os
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(8 << 20), b''):
            h.update(block)
    return h.hexdigest()


p = argparse.ArgumentParser(description=__doc__)
for key in ('bundle', 'checker', 'root', 'evidence', 'label', 'corpus', 'sa', 'remap'):
    p.add_argument('--' + key, required=True)
p.add_argument('--column')
p.add_argument('--isa-sample')
p.add_argument('--debug-column')
p.add_argument('--sha-expect', required=True)
p.add_argument('--chunk', type=int, default=4194304)
p.add_argument('--product-only', action='store_true')
p.add_argument('--wit-ram-budget', type=int)
p.add_argument('--extra-selftest', action='append', default=[])
p.add_argument('--chi', type=int, required=True)
p.add_argument('--runs', type=int, required=True)
p.add_argument('--threads', type=int, default=96)
a = p.parse_args()
root, evidence, bundle = Path(a.root).resolve(), Path(a.evidence).resolve(), Path(a.bundle).resolve()
root.mkdir(parents=True, exist_ok=False)
evidence.mkdir(parents=True, exist_ok=True)
result = dict(label=a.label, root=str(root), bundle=str(bundle), commands=[],
              input_sizes={key: Path(getattr(a, key.replace('-', '_'))).stat().st_size
                           for key in ('corpus', 'sa', 'column', 'isa-sample', 'remap')
                           if getattr(a, key.replace('-', '_'), None)},
              product_dump_scratch_bytes=0,
              timing_qualification='correctness gate only; idle box and N>=3 A/B not established',
              debug_stream_checked=not a.product_only)
result['source_manifest_sha256'] = sha(bundle / 'MANIFEST.sha256')
result['binaries_sha256'] = {name: sha(bundle/name) for name in ('cross_lcp_merge', 'xsa_finish')}
result['checker_sha256'] = sha(a.checker)
# Corpus length alone does not identify the SA that a column describes.
# This fast preflight rejects a foreign column before starting an expensive
# gate. Full product hash, closure and independent checker remain mandatory.
def preflight_column(path):
    with open(a.corpus, 'rb') as tf, open(a.sa, 'rb') as sf, open(path, 'rb') as cf:
        with mmap.mmap(tf.fileno(), 0, access=mmap.ACCESS_READ) as t, \
             mmap.mmap(sf.fileno(), 0, access=mmap.ACCESS_READ) as sa, \
             mmap.mmap(cf.fileno(), 0, access=mmap.ACCESS_READ) as col:
            n = len(t)
            magic, cn, rows, escapes, _, flags = struct.unpack_from('<6Q', col)
            assert magic == 5424044115 and cn == rows == n and flags == 1
            assert len(col) == 48 + n + 16 * escapes
            table = Path(a.remap).read_bytes()
            rng = random.Random(9181)
            ranks = sorted(set([min(n - 1, x) for x in (1, 2, 10, 100, 1000, 10000, 65536, 100000)] +
                               [rng.randrange(1, n) for _ in range(512)])) if n > 1 else []
            for r in ranks:
                left, right = struct.unpack_from('<QQ', sa, 48 + 8 * (r - 1))
                assert left < n and right < n
                got = col[48 + r]
                if got == 255:
                    lo, hi = 0, escapes
                    while lo < hi:
                        mid = (lo + hi) // 2
                        if struct.unpack_from('<Q', col, 48 + n + 16 * mid)[0] < r: lo = mid + 1
                        else: hi = mid
                    assert lo < escapes
                    rank, got = struct.unpack_from('<QQ', col, 48 + n + 16 * lo)
                    assert rank == r
                want, cap = 0, min(n - left, n - right)
                while want < cap and table[t[left + want]] == table[t[right + want]]: want += 1
                assert got == want, ('foreign/incompatible LCP column', path, r, got, want)
    return dict(path=path, sampled_adjacent_ranks=len(ranks), mismatches=0, seed=9181, complete_proof=False)

result['column_preflight'] = [preflight_column(path) for path in sorted(set(
    str(p) for p in (a.column, a.debug_column) if p))]

# Exclude corpus/remap and final output from scratch. Include pre-existing SA,
# column and sampled inverse explicitly; they remain live throughout this gate.
fixed_scratch = sum(result['input_sizes'].get(k, 0) for k in ('sa', 'column', 'isa-sample'))


def snapshot(directory):
    files = {}
    for f in directory.rglob('*'):
        if not f.is_file() or f.name.startswith('product.sxi2'):
            continue
        try:
            files[str(f.relative_to(directory))] = f.stat().st_size
        except FileNotFoundError:
            pass
    return sum(files.values()), files


def run(label, command, env=None, monitor=None):
    log = root / (label + '.log')
    start = time.monotonic()
    peak, peak_files, samples = 0, {}, 0
    cpu_before, cpu_last_progress = -1, start
    with log.open('w') as out:
        proc = subprocess.Popen(list(map(str, command)), env=env, stdout=out, stderr=subprocess.STDOUT)
        while True:
            if monitor:
                size, files = snapshot(monitor)
                samples += 1
                if size > peak:
                    peak, peak_files = size, files
            if label.endswith('-finish') and Path(f'/proc/{proc.pid}/stat').exists():
                try:
                    fields = Path(f'/proc/{proc.pid}/stat').read_text().split()
                    ticks = int(fields[13]) + int(fields[14])
                    if ticks != cpu_before:
                        cpu_before, cpu_last_progress = ticks, time.monotonic()
                    if time.monotonic() - cpu_last_progress > 60:
                        proc.kill()
                        proc.wait()
                        raise RuntimeError('finish made no CPU progress for 60s: ' + str(log))
                except FileNotFoundError:
                    pass
            code = proc.poll()
            if code is not None:
                break
            time.sleep(1)
    entry = dict(label=label, command=list(map(str, command)), returncode=code,
                 seconds=round(time.monotonic()-start, 3), log=str(log))
    if monitor:
        entry.update(observed_scratch_highwater_bytes=peak, scratch_samples=samples,
                     highwater_files=peak_files)
    result['commands'].append(entry)
    (evidence / (a.label + '-results.json')).write_text(json.dumps(result, indent=2)+'\n')
    assert code == 0, (label, code, str(log))
    return log.read_text()


clean = {k: v for k, v in os.environ.items() if not k.startswith(('XSA_FINISH_', 'CROSS_'))}
for mode in (('product',) if a.product_only else ('product', 'debug')):
    d = root / mode
    d.mkdir()
    prefix = d / 'frag'
    emission = run(mode+'-emit', [bundle/'cross_lcp_merge', '--binbwt', a.corpus,
                   '--remap', a.remap, '--out-prefix', prefix, '--threads', a.threads,
                   '--tape-emit', a.sa, '--tape-emit-chunk', a.chunk],
                   dict(clean, CROSS_V2_PIECE_WINDOW='100000000'), d)
    assert 'TAPE_EMIT_DONE' in emission
    assert Path(str(prefix)+'.pieces').exists() and Path(str(prefix)+'.rlebwt').is_file()
    size, files = snapshot(d)
    result[mode+'_emit_done_scratch_bytes'] = size
    result[mode+'_emit_then_finish_verified'] = True
    cmd = [bundle/'xsa_finish', '--prefix', prefix, '--corpus', a.corpus,
           '--remap', a.remap, '--output', d/'product.sxi2', '--threads', a.threads,
           '--chi-expect', a.chi, '--sample-count', '64']
    column = a.column or (a.debug_column if mode == 'debug' else None)
    if column: cmd += ['--lcp-column', column]
    if a.isa_sample: cmd += ['--isa-sample', a.isa_sample]
    if a.wit_ram_budget is not None: cmd += ['--wit-ram-budget', a.wit_ram_budget]
    env = dict(clean)
    if mode == 'debug':
        cmd += ['--debug-artifacts']
        env['XSA_FINISH_DUMP_STREAM'] = str(root/'band-stream.bin')
    finish = run(mode+'-finish', cmd, env, d)
    closure = re.findall(r'XSA_FINISH_KASAI_CLOSURE .*', finish)
    assert len(closure) == int(bool(column)), closure
    for counter in ('rank_holes', 'disorder', 'noncontiguous_runs', 'walk_fallbacks'):
        if column: assert re.search(r'\b'+counter+r'=0\b', closure[0]), closure
    assert f'XSA_FINISH_DONE chi={a.chi} n={Path(a.corpus).stat().st_size} r={a.runs} ' in finish
    assert f'XSA_FINISH_DEBUG_ARTIFACTS enabled={int(mode == "debug")} product_dump_scratch_bytes=0' in finish
    result[mode+'_sha256'] = sha(d/'product.sxi2')
    assert result[mode+'_sha256'] == a.sha_expect, result[mode+'_sha256']
    result[mode+'_closure'] = closure[0] if column else 'walk arm: unconditional stream extent/stamp guards; Kasai counters not applicable'
    result[mode+'_runs'] = a.runs
    result[mode+'_chi'] = a.chi
    if mode == 'product':
        assert not (root/'band-stream.bin').exists()
        product_high = max(x.get('observed_scratch_highwater_bytes', 0) for x in result['commands'])
        result['observed_product_working_scratch_highwater_bytes'] = product_high
        result['fixed_input_scratch_bytes'] = fixed_scratch
        result['observed_total_product_scratch_highwater_bytes'] = fixed_scratch + product_high
        result['scratch_measurement'] = '1s stat sampling; logical lengths; observed lower bound on peak; excludes corpus/remap/output, includes supplied SA/column/isamp'
    cmd = [a.checker, '--corpus', a.corpus, '--remap', a.remap, '--sa', a.sa,
           '--container', d/'product.sxi2']
    if mode == 'debug':
        cmd += ['--stream', root/'band-stream.bin']
        if column:
            cmd += ['--chi-expect', a.chi, '--chi-column', column]
    checked = run(mode+'-check', cmd)
    assert f'rows={Path(a.corpus).stat().st_size} runs={a.runs} mismatches=0 ' in checked
    if mode == 'debug':
        assert checked.count('mismatches=0 ') == 2
    (evidence/(a.label+'-'+mode+'-check.log')).write_text(checked)
    summary = '\n'.join(line for line in finish.splitlines() if any(key in line for key in (
        'DEBUG_ARTIFACTS', 'KASAI_CLOSURE', 'KASAI_ANCHORS', 'SEAM_ANCHORS',
        'chi-rspace:', 'ENDPOINT_STREAM_PASS', 'XSA_FINISH_DONE', 'SXI2_PASS',
        'SWEEP_CYCLES', 'SWEEP_PHASE_WALLS', 'SWEEP_STATS', 'SWEEP_ANATOMY',
        'CORPUS_ACCESS phase=sweep', 'XSA_FINISH_REGION')))
    (evidence/(a.label+'-'+mode+'-finish-summary.log')).write_text(summary+'\n')
    (evidence/(a.label+'-'+mode+'-emit.log')).write_text(emission)
if not a.product_only:
    assert result['product_sha256'] == result['debug_sha256'], 'debug changes product bytes'
for flag in ['--selftest-stream-guard', '--selftest-repair-chunks', '--selftest-sweep-monoid'] + a.extra_selftest:
    run(flag[2:], [bundle/'xsa_finish', flag])
run('selftest-seam-spool', [bundle/'xsa_finish', '--selftest-seam-spool', root/'selftest-spool'])
run('checker-selftest', [a.checker, '--selftest'])
result['passed'] = True
(evidence/(a.label+'-results.json')).write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps({k: result[k] for k in ('label', 'passed', 'product_sha256', 'product_chi',
    'product_runs', 'observed_total_product_scratch_highwater_bytes')}, indent=2))
