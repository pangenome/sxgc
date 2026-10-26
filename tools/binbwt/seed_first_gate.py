#!/usr/bin/env python3
"""Reproducible seed-first gate using a Cargo-packaged bundle, never the bank.

Emission must exit successfully, publish both artifacts, and journal DONE
before finish starts. Separate product and explicit-debug runs attest zero
product dump scratch and compare the band stream and published m1 to SA.
"""
import argparse
import hashlib
import json
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
for key in ('bundle', 'checker', 'root', 'evidence', 'label', 'corpus', 'sa', 'column', 'isa-sample', 'remap'):
    p.add_argument('--' + key, required=True)
p.add_argument('--chi', type=int, required=True)
p.add_argument('--runs', type=int, required=True)
p.add_argument('--threads', type=int, default=96)
a = p.parse_args()
root, evidence, bundle = Path(a.root).resolve(), Path(a.evidence).resolve(), Path(a.bundle).resolve()
root.mkdir(parents=True, exist_ok=False)
evidence.mkdir(parents=True, exist_ok=True)
result = dict(label=a.label, root=str(root), bundle=str(bundle), commands=[],
              input_sizes={key: Path(getattr(a, key.replace('-', '_'))).stat().st_size
                           for key in ('corpus', 'sa', 'column', 'isa-sample', 'remap')},
              product_dump_scratch_bytes=0)
result['source_manifest_sha256'] = sha(bundle / 'MANIFEST.sha256')
result['binaries_sha256'] = {name: sha(bundle/name) for name in ('cross_lcp_merge', 'xsa_finish')}
result['checker_sha256'] = sha(a.checker)
# Exclude corpus/remap and final output from scratch. Include pre-existing SA,
# column and sampled inverse explicitly; they remain live throughout this gate.
fixed_scratch = sum(result['input_sizes'][k] for k in ('sa', 'column', 'isa-sample'))


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
    with log.open('w') as out:
        proc = subprocess.Popen(list(map(str, command)), env=env, stdout=out, stderr=subprocess.STDOUT)
        while True:
            if monitor:
                size, files = snapshot(monitor)
                samples += 1
                if size > peak:
                    peak, peak_files = size, files
            code = proc.poll()
            if code is not None:
                break
            time.sleep(.1)
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
for mode in ('product', 'debug'):
    d = root / mode
    d.mkdir()
    prefix = d / 'frag'
    emission = run(mode+'-emit', [bundle/'cross_lcp_merge', '--binbwt', a.corpus,
                   '--remap', a.remap, '--out-prefix', prefix, '--threads', a.threads,
                   '--tape-emit', a.sa, '--tape-emit-chunk', '16777216'],
                   dict(clean, CROSS_V2_PIECE_WINDOW='100000000'), d)
    assert 'TAPE_EMIT_DONE' in emission
    assert Path(str(prefix)+'.pieces').exists() and Path(str(prefix)+'.rlebwt').is_file()
    size, files = snapshot(d)
    result[mode+'_emit_done_scratch_bytes'] = size
    result[mode+'_emit_then_finish_verified'] = True
    cmd = [bundle/'xsa_finish', '--prefix', prefix, '--corpus', a.corpus,
           '--remap', a.remap, '--output', d/'product.sxi2', '--threads', a.threads,
           '--lcp-column', a.column, '--isa-sample', a.isa_sample,
           '--chi-expect', a.chi, '--sample-count', '64']
    env = dict(clean)
    if mode == 'debug':
        cmd += ['--debug-artifacts']
        env['XSA_FINISH_DUMP_STREAM'] = str(root/'band-stream.bin')
    finish = run(mode+'-finish', cmd, env, d)
    closure = re.findall(r'XSA_FINISH_KASAI_CLOSURE .*', finish)
    assert len(closure) == 1
    for counter in ('rank_holes', 'disorder', 'noncontiguous_runs', 'walk_fallbacks'):
        assert re.search(r'\b'+counter+r'=0\b', closure[0]), closure
    assert f'XSA_FINISH_DONE chi={a.chi} n={Path(a.corpus).stat().st_size} r={a.runs} ' in finish
    assert f'XSA_FINISH_DEBUG_ARTIFACTS enabled={int(mode == "debug")} product_dump_scratch_bytes=0' in finish
    result[mode+'_sha256'] = sha(d/'product.sxi2')
    result[mode+'_closure'] = closure[0]
    result[mode+'_runs'] = a.runs
    result[mode+'_chi'] = a.chi
    if mode == 'product':
        assert not (root/'band-stream.bin').exists()
        product_high = max(x.get('observed_scratch_highwater_bytes', 0) for x in result['commands'])
        result['observed_product_working_scratch_highwater_bytes'] = product_high
        result['fixed_input_scratch_bytes'] = fixed_scratch
        result['observed_total_product_scratch_highwater_bytes'] = fixed_scratch + product_high
        result['scratch_measurement'] = '100ms stat sampling; logical lengths; observed lower bound on peak; excludes corpus/remap/output, includes supplied SA/column/isamp'
    cmd = [a.checker, '--corpus', a.corpus, '--remap', a.remap, '--sa', a.sa,
           '--container', d/'product.sxi2']
    if mode == 'debug':
        cmd += ['--stream', root/'band-stream.bin', '--chi-expect', a.chi]
        if a.label != 'fixture':
            cmd += ['--chi-column', a.column]
    checked = run(mode+'-check', cmd)
    assert f'rows={Path(a.corpus).stat().st_size} runs={a.runs} mismatches=0 ' in checked
    if mode == 'debug':
        assert checked.count('mismatches=0 ') == 2
    (evidence/(a.label+'-'+mode+'-check.log')).write_text(checked)
    summary = '\n'.join(line for line in finish.splitlines() if any(key in line for key in (
        'DEBUG_ARTIFACTS', 'KASAI_CLOSURE', 'KASAI_ANCHORS', 'SEAM_ANCHORS',
        'chi-rspace:', 'ENDPOINT_STREAM_PASS', 'XSA_FINISH_DONE', 'SXI2_PASS')))
    (evidence/(a.label+'-'+mode+'-finish-summary.log')).write_text(summary+'\n')
    (evidence/(a.label+'-'+mode+'-emit.log')).write_text(emission)
assert result['product_sha256'] == result['debug_sha256'], 'debug changes product bytes'
result['passed'] = True
(evidence/(a.label+'-results.json')).write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps({k: result[k] for k in ('label', 'passed', 'product_sha256', 'product_chi',
    'product_runs', 'observed_total_product_scratch_highwater_bytes')}, indent=2))
