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
from scratch_components import snapshot

p = argparse.ArgumentParser(description=__doc__)
for name in ('bundle', 'sorter', 'root', 'corpus', 'remap'):
    p.add_argument('--' + name, required=True)
for name in ('sa', 'column', 'isa-sample', 'sha-expect', 'chi', 'runs', 'baseline'):
    p.add_argument('--' + name)
p.add_argument('--evidence-dir', help='write full component time series and peak decomposition here')
p.add_argument('--threads', type=int, default=96)
p.add_argument('--pending-bands', type=int, default=8)
p.add_argument('--wave-cap-gb', type=float)
p.add_argument('--produce-column', action='store_true')
p.add_argument('--produce-isa', action='store_true')
p.add_argument('--isa-stride', type=int, default=1024)
p.add_argument('--debug', action='store_true')
p.add_argument('--audit-column', action='store_true', help='explicit debug/audit arm only')
p.add_argument('--piece-window', type=int, help='override automatic 0.15n piece credit budget')
p.add_argument('--selftests', action='store_true')
p.add_argument('--sample-checker', default='/tmp/sa_check_fused')
p.add_argument('--checker', default='/tmp/stream_vs_bwt')
a = p.parse_args()
if (a.column or a.isa_sample or a.produce_column or a.produce_isa) and not a.audit_column:
    p.error('column and ISA are restricted to --audit-column')
if a.produce_column or a.produce_isa:
    p.error('concurrent finish requires audit columns to exist before launch; supply read-only fixtures')
n = Path(a.corpus).stat().st_size
if n > 10_000_017_258:
    p.error('operator hold: corpus exceeds the accepted 10 GB fixture; whole-run peak must be bounded before larger runs')
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
# Credit includes packed endpoints, lengths, characters and BWT words.
# The BWT replay remains separately measured: its file is retained to EOF.
piece_window = a.piece_window if a.piece_window is not None else max(
    2, int(0.15*n / (65536*(9+2*((n+9).bit_length()/8)))))
if piece_window < 2: p.error('piece window must be >=2 for seam lookahead')
result['piece_window'] = piece_window
env = dict(clean, CROSS_V2_PIECE_WINDOW=str(piece_window))
processes = []
samples = (root / 'du.tsv').open('w', buffering=1)
samples.write('elapsed\tphase\torder_allocated\tall_allocated\tintermediate_allocated\tfree\n')
evidence = Path(a.evidence_dir) if a.evidence_dir else root
evidence.mkdir(parents=True, exist_ok=True)
component_samples = (evidence/'scratch-components.jsonl').open('w', buffering=1)
result['component_peaks'] = {}
start = time.monotonic()
last_sample_started = start

def save():
    if 'whole_peak_sample' in result:
        peak = result['whole_peak_sample']
        table = ['| Component | Bytes at whole peak | n at whole peak | Independent peak bytes | Independent peak / n |', '|---|---:|---:|---:|---:|']
        for key in sorted(result['component_peaks']):
            value = peak['components'].get(key, 0)
            high = result['component_peaks'][key]
            table.append(f'| {key} | {value} | {value/n:.9f} | {high} | {high/n:.9f} |')
        table.append(f"| TOTAL | {peak['total_bytes']} | {peak['total_bytes']/n:.9f} | | |")
        (evidence/'scratch-decomposition.md').write_text('\n'.join(table)+'\n')
    (root/'results.json').write_text(json.dumps(result, indent=2)+'\n')

def sample(phase):
    global last_sample_started
    last_sample_started = time.monotonic()
    o, total, parts = snapshot(root)
    wire = parts.get('pieces', 0) + parts.get('rlebwt', 0)
    result['peak_live_wire_bytes'] = max(result.get('peak_live_wire_bytes', 0), wire)
    result['peak_live_wire_over_n'] = result['peak_live_wire_bytes']/n
    elapsed = round(time.monotonic()-start, 3)
    record = dict(elapsed=elapsed, phase=phase, total_bytes=total, components=parts,
                  traversal_seconds=round(time.monotonic()-last_sample_started, 6))
    component_samples.write(json.dumps(record, sort_keys=True)+'\n')
    for key, value in parts.items():
        result['component_peaks'][key] = max(result['component_peaks'].get(key, 0), value)
    if total >= result['peak_all_allocated']:
        result['whole_peak_sample'] = record
    excluded = parts.get('container', 0) + parts.get('debug_dump', 0)
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
        time.sleep(max(0.0, last_sample_started+1-time.monotonic()))
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
    if a.produce_isa:
        a.isa_sample = str(order/'isa-sample')
        cmd += ['--isa-sample-out', a.isa_sample, '--isa-sample-k', a.isa_stride]
    if a.wave_cap_gb: cmd += ['--scratch-cap-gb', a.wave_cap_gb]
    order_cmd = cmd
    cmd = [bundle/'xsa_finish', '--prefix', prefix, '--corpus', a.corpus, '--remap', a.remap,
           '--output', product/'product.sxi2', '--threads', a.threads, '--sample-count', '64']
    if a.chi: cmd += ['--chi-expect', a.chi]
    if a.column: cmd += ['--lcp-column', a.column]
    if a.isa_sample: cmd += ['--isa-sample', a.isa_sample]
    fe = dict(clean)
    if a.debug:
        cmd += ['--debug-artifacts']
        fe['XSA_FINISH_DUMP_STREAM'] = str(root/'band-stream.bin')
    # Start the reader first, then immediately start both upstream stages.
    finisher = spawn('finish', cmd, fe)
    producer = spawn('order', order_cmd, env)
    consumer = spawn('emit', [bundle/'cross_lcp_merge', '--binbwt', a.corpus, '--remap', a.remap,
                     '--out-prefix', prefix, '--threads', a.threads, '--tape-bands', order/'bands'], env)
    pending = [finisher, producer, consumer]
    texts = {}
    last_progress = time.monotonic()
    last_signature = None
    while pending:
        sample('order+emit+finish')
        for item in pending[:]:
            if item[0].poll() is not None:
                texts[item[1]['label']] = finish_entry(item)
                pending.remove(item)
        # CPU ticks and I/O counters detect a live but parked producer as well
        # as waiting consumers. A dead emitter is reaped above and fails fast.
        signature = []
        for proc, entry, log in pending:
            try:
                fields = Path(f'/proc/{proc.pid}/stat').read_text().split()
                io = dict(line.split(': ', 1) for line in
                          Path(f'/proc/{proc.pid}/io').read_text().splitlines())
                # Polling read syscalls are not useful progress. Quantize CPU
                # to whole seconds so an idle polling loop cannot reset this.
                cpu_seconds = (int(fields[13])+int(fields[14])) // os.sysconf('SC_CLK_TCK')
                signature.append((proc.pid, cpu_seconds, io['wchar'], io['write_bytes']))
            except (FileNotFoundError, ProcessLookupError, PermissionError):
                if proc.poll() is None:
                    raise  # unreadable live process is not evidence of progress
        if signature != last_signature:
            last_progress = time.monotonic()
            last_signature = signature
        elif time.monotonic() - last_progress > 60:
            raise RuntimeError('pipeline parked for 60s: no CPU or I/O progress; terminating all stages')
        if pending: time.sleep(max(0.0, last_sample_started+1-time.monotonic()))
    assert 'SS_DONE' in texts['order'] and 'TAPE_EMIT_DONE' in texts['emit']
    assert not list((order/'bands').glob('*.sa')), 'unconsumed bands'
    assert not list((order/'waves').iterdir()), 'unconsumed scatter parts'
    assert not list(Path(str(prefix)+'.pieces').glob('p-*')), 'unconsumed pieces'
    result['no_full_sa_or_sym_generated'] = True
    run('sample-order', [a.sample_checker, '--corpus', a.corpus, '--remap', a.remap,
                         '--sa', root/'order-sample.sxsa', '--sample-first-rank', sample_lo])
    result['sample'] = dict(first_rank=sample_lo, count=sample_count, adjacent_pairs=sample_count-1,
                            global_permutation_checked=False)
    text = texts['finish']
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
    result['passed_scope'] = 'byte product and enabled audits; space acceptance evaluated separately'
    result['space_acceptance_passed'] = (result['peak_all_allocated'] <= 2*n and
                                         result['peak_live_wire_bytes'] <= 0.5*n)
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
    component_samples.close()
