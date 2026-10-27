#!/usr/bin/env python3
"""Run the bounded tape pipeline, sample allocated scratch with du each second.

External SA/column options are validation-only read-only inputs; producer and
emitter never receive an SA path. No performance comparison is claimed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
for name in ('bundle', 'sorter', 'root', 'corpus', 'remap'):
    p.add_argument('--' + name, required=True)
for name in ('sa', 'column', 'isa-sample', 'sha-expect', 'chi', 'runs', 'baseline'):
    p.add_argument('--' + name)
p.add_argument('--threads', type=int, default=96)
p.add_argument('--pending-bands', type=int, default=8)
p.add_argument('--wave-cap-gb', type=float)
p.add_argument('--produce-column', action='store_true')
p.add_argument('--debug', action='store_true')
p.add_argument('--selftests', action='store_true')
p.add_argument('--sample-checker', default='/tmp/sa_check_fused')
p.add_argument('--checker', default='/tmp/stream_vs_bwt')
a = p.parse_args()
root = Path(a.root).resolve()
root.mkdir(parents=True, exist_ok=False)
order = root / 'order'
order.mkdir()
(order / 'bands').mkdir()
(order / 'waves').mkdir()
product = root / 'product'
product.mkdir()
bundle = Path(a.bundle).resolve()
prefix = product / 'frag'
n = Path(a.corpus).stat().st_size
sample_count = min(4096, n)
sample_lo = (n - sample_count) // 2
result = dict(n=n, K=a.pending_bands, source_manifest=json.loads((bundle/'MANIFEST.sha256').read_text())['source_sha256'],
              commands=[], peak_order_allocated=0, peak_all_allocated=0,
              peak_intermediate_allocated=0, scratch_sample_period_seconds=1,
              scratch_scope='order=waves+unconsumed packed bands+optional generated column; all=entire new run directory; intermediate=all minus container and explicit debug dump; read-only validation fixtures excluded',
              timing_qualification='single-run observations; no paired N>=3 A/B; speed effects unverifiable')
def file_sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for block in iter(lambda: f.read(8<<20), b''): h.update(block)
    return h.hexdigest()

result['binary_sha256'] = {name: file_sha(bundle/name) for name in ('cross_lcp_merge', 'xsa_finish')}
result['sorter_sha256'] = file_sha(a.sorter)
result['sample_checker_sha256'] = file_sha(a.sample_checker)
clean = {k: v for k, v in os.environ.items() if not k.startswith(('CROSS_', 'XSA_FINISH_'))}
env = dict(clean, CROSS_V2_PIECE_WINDOW='100000000')
processes = []
samples = (root / 'du.tsv').open('w', buffering=1)
samples.write('elapsed\tphase\torder_allocated\tall_allocated\tintermediate_allocated\tfree\n')
start = time.monotonic()

def save():
    (root/'results.json').write_text(json.dumps(result, indent=2)+'\n')

def du(path):
    # Files can disappear during traversal; du still reports their allocated size.
    r = subprocess.run(['du', '-s', '-B1', str(path)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return int(r.stdout.split()[0]) if r.stdout else 0

def sample(phase):
    o, total = du(order), du(root)
    excluded = 0
    for f in [product/'product.sxi2', root/'band-stream.bin']:
        try: excluded += f.stat().st_blocks*512
        except FileNotFoundError: pass
    intermediate = max(0, total - excluded)
    free = shutil.disk_usage(root).free
    samples.write(f'{time.monotonic()-start:.3f}\t{phase}\t{o}\t{total}\t{intermediate}\t{free}\n')
    for key, value in [('peak_order_allocated', o), ('peak_all_allocated', total), ('peak_intermediate_allocated', intermediate)]:
        result[key] = max(result[key], value)
    # Abort before ENOSPC. This is an additional operational guard, not an audit replacement.
    if free < max(2_000_000_000, n//20):
        raise RuntimeError('disk reserve reached; stopped before ENOSPC')

def spawn(label, cmd, e=clean):
    cmd = list(map(str, cmd))
    log = (root/(label+'.log')).open('w')
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=e)
    entry = dict(label=label, command=cmd, pid=proc.pid, started=time.monotonic())
    processes.append((proc, entry, log))
    result['commands'].append(entry)
    save()
    return proc, entry, log

def finish_entry(item):
    proc, entry, log = item
    entry['returncode'] = proc.returncode
    entry['seconds'] = round(time.monotonic()-entry.pop('started'), 3)
    log.close()
    save()
    assert proc.returncode == 0, (entry['label'], proc.returncode)
    return (root/(entry['label']+'.log')).read_text()

def run(label, cmd, e=clean):
    item = spawn(label, cmd, e)
    while item[0].poll() is None:
        sample(label)
        time.sleep(1)
    return finish_entry(item)

try:
    # 100 GB observed downstream scratch is about 6.5n; plus a <=2n wave,
    # output and reserve. Refuse an obviously impossible launch.
    if n > 50_000_000_000 and shutil.disk_usage(root).free < 9.0*n:
        raise RuntimeError('preflight requires 9n free for unchanged downstream plus bounded order')
    cmd = [a.sorter, '--corpus', a.corpus, '--remap', a.remap, '--scratch', order/'waves',
           '--tape-bands', order/'bands', '--pending-bands', a.pending_bands,
           '--ram-gb', '48', '--par-bands', '8', '--threads', a.threads,
           '--order-sample-out', root/'order-sample.sxsa', '--order-sample-lo', sample_lo,
           '--order-sample-count', sample_count]
    if a.produce_column:
        a.column = str(order/'column')
        cmd += ['--lcp-out', a.column]
    if a.wave_cap_gb: cmd += ['--scratch-cap-gb', a.wave_cap_gb]
    producer = spawn('order', cmd, env)
    consumer = spawn('emit', [bundle/'cross_lcp_merge', '--binbwt', a.corpus, '--remap', a.remap,
                     '--out-prefix', prefix, '--threads', a.threads, '--tape-bands', order/'bands'], env)
    pending = [producer, consumer]
    texts = {}
    while pending:
        sample('order+emit')
        for item in pending[:]:
            if item[0].poll() is not None:
                texts[item[1]['label']] = finish_entry(item)
                pending.remove(item)
        if pending: time.sleep(1)
    assert 'SS_DONE' in texts['order'] and 'TAPE_EMIT_DONE' in texts['emit']
    assert not list((order/'bands').glob('*.sa')), 'unconsumed bands'
    assert not list((order/'waves').iterdir()), 'unconsumed scatter parts'
    result['no_full_sa_or_sym_generated'] = True
    run('sample-order', [a.sample_checker, '--corpus', a.corpus, '--remap', a.remap,
                         '--sa', root/'order-sample.sxsa', '--sample-first-rank', sample_lo])
    result['sample'] = dict(first_rank=sample_lo, count=sample_count, adjacent_pairs=sample_count-1,
                            global_permutation_checked=False)
    cmd = [bundle/'xsa_finish', '--prefix', prefix, '--corpus', a.corpus, '--remap', a.remap,
           '--output', product/'product.sxi2', '--threads', a.threads, '--sample-count', '64']
    if a.chi: cmd += ['--chi-expect', a.chi]
    if a.column: cmd += ['--lcp-column', a.column]
    if a.isa_sample: cmd += ['--isa-sample', a.isa_sample]
    fe = dict(clean)
    if a.debug:
        cmd += ['--debug-artifacts']
        fe['XSA_FINISH_DUMP_STREAM'] = str(root/'band-stream.bin')
    text = run('finish', cmd, fe)
    assert 'product_dump_scratch_bytes=0' in text
    if a.column:
        closure = next(line for line in text.splitlines() if 'XSA_FINISH_KASAI_CLOSURE' in line)
        for name in ('rank_holes', 'disorder', 'noncontiguous_runs', 'walk_fallbacks'):
            assert re.search(r'\b'+name+r'=0\b', closure), closure
        result['closure'] = closure
    else: result['closure'] = 'walk arm; no rank column supplied'
    done = re.search(r'XSA_FINISH_DONE chi=(\d+) n=(\d+) r=(\d+)', text)
    assert done and int(done[2]) == n
    result.update(chi=int(done[1]), runs=int(done[3]))
    if a.runs: assert result['runs'] == int(a.runs)
    h = hashlib.sha256()
    with (product/'product.sxi2').open('rb') as f:
        for block in iter(lambda: f.read(8<<20), b''): h.update(block)
    result['sha256'] = h.hexdigest()
    if a.sha_expect: assert result['sha256'] == a.sha_expect
    if a.baseline: assert subprocess.run(['cmp', '-s', product/'product.sxi2', a.baseline]).returncode == 0
    if a.sa:
        cmd = [a.checker, '--corpus', a.corpus, '--remap', a.remap, '--sa', a.sa,
               '--container', product/'product.sxi2']
        if a.debug: cmd += ['--stream', root/'band-stream.bin']
        text = run('check', cmd)
        assert f'rows={n} runs={result["runs"]} mismatches=0 ' in text
    if a.selftests:
        for flag in ('stream-guard', 'repair-chunks', 'sweep-monoid', 'packed-bits', 'witness-batch', 'seam-spool'):
            cmd = [bundle/'xsa_finish', '--selftest-'+flag]
            if flag == 'seam-spool': cmd += [root/'selftest-spool']
            run('selftest-'+flag, cmd)
        run('checker-selftest', [a.checker, '--selftest'])
    result['peak_order_over_n'] = result['peak_order_allocated']/n
    result['peak_intermediate_over_n'] = result['peak_intermediate_allocated']/n
    result['passed'] = True
    sample('done')
    save()
    print(json.dumps(result, indent=2))
except BaseException as error:
    result['error'] = repr(error)
    for proc, entry, log in processes:
        if proc.poll() is None: proc.terminate()
    for proc, entry, log in processes:
        try: proc.wait(timeout=5)
        except subprocess.TimeoutExpired: proc.kill(); proc.wait()
    save()
    raise
finally:
    samples.close()
