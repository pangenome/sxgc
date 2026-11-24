"""Small only. Run in a fresh constrained systemd scope (see accompanying note)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import signal
import struct
import subprocess as sp
import sys
import time

if not __debug__:
    raise SystemExit("fixture assertions require python without -O")

args = argparse.ArgumentParser()
args.add_argument('directory', type=Path)
args.add_argument('tools', type=Path)
args.add_argument('--native-tools', type=Path)
args.add_argument('--wire', choices=['narrow', 'row43'], default='narrow')
args.add_argument('--stream-timeout', type=int, help='shorten native pipeline watchdog (1..240 seconds)')
opts = args.parse_args()
D, tools = opts.directory.resolve(), opts.tools.resolve()
native = opts.native_tools.resolve() if opts.native_tools else None
wide = opts.wire == 'row43'
if wide and not native: args.error('row43 needs --native-tools')
if opts.stream_timeout is not None and not (native and 1 <= opts.stream_timeout <= 240):
    args.error('--stream-timeout requires --native-tools and a limit in [1,240]')
D.mkdir(mode=0o700)  # fresh, never recursively delete evidence
G = Path(__file__).resolve().parent
cg = Path('/sys/fs/cgroup' + Path('/proc/self/cgroup').read_text().strip().split('::')[1])
assert (cg/'memory.max').read_text().strip() == str(3 << 30)
assert (cg/'memory.swap.max').read_text().strip() == '0'
assert (cg/'cpu.max').read_text().strip() == '200000 100000'
sp.run(['sha256sum', '--strict', '-c', 'fixture-tools.sha256'], cwd=tools, check=True)
if native:
    sp.run(['sha256sum', '--strict', '-c', 'native-tools.sha256'], cwd=native, check=True)
X = tools/'xsa'
F = Path.home()/'.cache/xsa/13524d6bbedad508fdd8e30e7ab709dbf62ae5db64eb0cd8864dd79efa5b7b4c/xsa_finish'
assert hashlib.sha256(F.read_bytes()).hexdigest() == 'bb929a7092cfeba7faa7c37ca999b71563e3af782d73fe693d1a35f5ecdbaaa9'
# Deliberate metadata cut in LEFTRIGHT, plus an internal occurrence. Lowercase
# pad keeps a reproducible alphabet, natural repeats and searchable short words.
C = D/'corpus'; M = D/'map'; M.mkdir()
a = b'abracadabra!\x1e' * 1536 + b'LEFT'
b = b'RIGHT---LEFTRIGHT' + random.Random(100).randbytes(6 << 20).translate(bytes(97+i%20 for i in range(256)))
n = len(a)+len(b); C.write_bytes(a+b)
# The first three bytes form a whole doc: abr's continuation is END, not a.
(M/'seps.bin').write_bytes(struct.pack('<4sIIIQQQQIIIQ', b'XSEP',1,1,64,3,n,1,0,0,3,len(a)-3,0))
cut = len(a); del a,b
if native:
    assert hashlib.sha256(C.read_bytes()).hexdigest() == '0bcf20cfcdb2ddc4bd49c52c25d6b4c810c0364fb924c8b54d71968c6cb07539'
    assert hashlib.sha256((M/'seps.bin').read_bytes()).hexdigest() == '7ad88538c8c85a80eca58591a187a27539dea9d988a80c86e7c544372befc8c3'
stop = D/'stop.requested'
base_env = {k:os.environ[k] for k in ('PATH','HOME','LANG') if k in os.environ}
base_env['RAYON_NUM_THREADS'] = '2'
workers = []; peak = 0; phases = {}


def start(tag, args, *, env=None, **kw):
    log = (D/f'{tag}.log').open('xb')
    print(tag, list(map(str,args)), env, flush=True)
    try:
        p = sp.Popen(args, cwd=D, env=base_env | (env or {}), stderr=log,
                     start_new_session=True, **({'stdout': log} | kw))
    finally:
        log.close()
    workers.append(p)
    return p


def wait(label=None):
    global peak
    local_peak = memory_peak = 0
    begin = time.monotonic()
    until = begin + ((opts.stream_timeout or 240) if label == 'candidate-stream' else 240)
    while True:
        if native and stop.exists(): raise InterruptedError("fixture stop requested")
        live = 0
        for root, _, files in os.walk(D):
            try: live += Path(root).stat().st_blocks*512
            except FileNotFoundError: continue
            for f in files:
                try: live += (Path(root)/f).stat().st_blocks*512
                except FileNotFoundError: pass
        peak = max(peak, live)
        local_peak = max(local_peak, live)
        memory_peak = max(memory_peak, int((cg/"memory.current").read_text()))
        assert live < 512 << 20, 'whole-fixture scratch alarm'
        rc = [p.poll() for p in workers]
        if any(v not in (None,0) for v in rc): raise RuntimeError(f'worker failure {rc}')
        if None not in rc: break
        if time.monotonic() > until: raise TimeoutError('stalled fixture')
        time.sleep(.04)
    workers.clear()
    if label:
        phases[label] = dict(wait_s=round(time.monotonic()-begin, 3), sampled_scratch=local_peak, sampled_memory=memory_peak)
        print(label, "sampled whole-fixture allocated", local_peak, "scope charged", memory_peak)


def product():
    ref = D/'ref'; cand = D/('candidate.partial.sxi2' if native else 'cand.sxi2'); prefix = D/'merged/frag'; bands = D/'bands'; tap = D/'tap'
    for p in (bands, prefix.parent): p.mkdir()
    if not native: tap.mkdir()
    start('ref',[X,'build','--tape','--input',C,'--scratch',ref,'--threads','1','--memory-gb','3',
                 '--scratch-budget-gb','1','--trie-depth','8','--trie-min-support','1000','--docs-map',M]); wait('reference')
    rfile = ref/'corpus.sxi2'; remap = ref/'remap-256.bin'
    common = ['--corpus',C,'--remap',remap,'--threads','1']
    start('tap',[X,'ctx-tap','--corpus',C,'--bands',tap,'--n',str(n+1),'--out',D/'m20',
                 '--depth','8','--min-support','1000','--docs-min-support','1000','--threads','1','--doc-starts',M])
    rows = start('rows',[native/'v10_rows' if wide else tools/'fixture_rows',C],env={'XSA_TEST_OVERLAP_BINNED':'1'},stdout=sp.PIPE)
    cmd = ([native/'relay', opts.wire, C, str(n), tap, stop] if native else
           [sys.executable,G/'overlap_v10_tap.py',tap,str(n)])
    tee = start('relay',cmd,stdin=rows.stdout,stdout=sp.PIPE)
    rows.stdout.close()
    start('sink',[(native if wide else tools)/'pfp_sort_r',*common,'--out-prefix',prefix,'--bands',bands,'--scratch-budget-bytes',str(128<<20)],
          env={'XSA_TEST_EXTERNAL_TRIPLES':'1','CROSS_V2_PIECES':'1','SS_LCP_SLICE_WINDOW':'8'},stdin=tee.stdout)
    tee.stdout.close()
    start('finish',[F,*common,'--prefix',prefix,'--lcp-bands',bands,'--output',cand,'--poll-timeout','200',
                    '--wit-ram-budget',str(128<<20),'--with-chi','--no-lcp-runs'],env={'XSA_WORK_ROOT':'driver'}); wait('candidate-stream')
    start('join',[X,'fixture-join',cand,D/'m20',M]); wait('candidate-join')
    assert {p.name for p in tap.iterdir()} == {'end', 'end.ready'}
    assert (tap/'end').read_text() == '193'
    expected = rfile.read_bytes(); got = cand.read_bytes(); assert expected == got, 'full container mismatch'
    v, = struct.unpack_from('<I',got,4); count, = struct.unpack_from('<I',got,32)
    ids = [struct.unpack_from('<I',got,len(got)-40*(count-i))[0] for i in range(count)]
    assert v == 10 and ids == [1,4,5,7,10,11,12,13,15,20]
    digest = hashlib.sha256(got).hexdigest()
    assert digest == '55eea26f9d735664204fb58f33f7c601477bfbecfb8f34d298e6425ce92bcb07'
    print('full container', v, ids, len(got), digest)
    for tag,path in [('ref',rfile),('cand',cand)]:
        start(tag+'-ctx',[X,'ctx-verify','--sxi',path,'--text',C,'--context','abr','--context','abra']); wait()
        start(tag+'-docs',[X,'docstarts','verify',path,'--docs-map',M,'--samples','128']); wait()
        assert 'exact=2 mismatches=0 below_min_support=0' in (D/f'{tag}-ctx.log').read_text()
        assert 'DOCSTARTS_PARITY result=PASS' in (D/f'{tag}-docs.log').read_text()
        for cross in (False,True):
            name = f'{tag}-search-{cross}'
            start(name,[X,'search',path,'--pattern','LEFTRIGHT','--jsonl','--corpus',C]+(['--cross-doc'] if cross else [])); wait()
            hits = [json.loads(l) for l in (D/f'{name}.log').read_text().splitlines() if l.startswith('{')]
            assert sorted((h['pos'],h['cross']) for h in hits) == sorted([(cut+8,0)]+([(cut-4,1)] if cross else []))

    if native:
        if stop.exists(): raise InterruptedError('stopped before verification receipt')
        cand.rename(D/'cand.sxi2')
        receipt = dict(phases=phases, wire=opts.wire, n=n, rows=n+1, verified_equal_bytes=len(got),
                       full_sha256=digest, members=ids, boundary_queries_passed=True,
                       relay=(D/'relay.log').read_text().strip(), native_tools=(native/'native-tools.sha256').read_text().splitlines(),
                       frozen_fixture=(tools/'fixture-tools.sha256').read_text().splitlines(),
                       finish_sha256=hashlib.sha256(F.read_bytes()).hexdigest(),
                       sampled_allocated_scratch_peak=peak, service_memory_peak=int((cg/'memory.peak').read_text()),
                       memory_max=3<<30, swap_max=0, cpu_quota_percent=200)
        with (D/'verification.json').open('x') as out: json.dump(receipt,out,indent=2); out.write('\n')

if native:
    signal.signal(signal.SIGTERM, lambda *_: stop.touch())
try:
    product()
finally:
    if native and workers:
        stop.touch()
        # Give blocked relay time to see STOP; closing its pipes wakes peers.
        # Notify each historical protocol that has a stop marker; no credit
        # or completion markers are synthesized. Escalate only after grace.
        for marker in ('tap/producer.interrupted', 'bands/producer.interrupted',
                       'merged/modeb/stop.requested'):
            path = D/marker
            if path.parent.is_dir(): path.touch()
        for sig in (None, signal.SIGTERM):
            if sig:
                for p in workers:
                    if p.poll() is None:
                        try: os.killpg(p.pid,sig)
                        except ProcessLookupError: pass
            until = time.monotonic()+1
            while any(p.poll() is None for p in workers) and time.monotonic()<until: time.sleep(.02)
    for p in workers:
        try: os.killpg(p.pid,signal.SIGKILL)
        except ProcessLookupError: pass
    for p in workers: p.wait()
    print('sampled_allocated_scratch_peak',peak,'scope_memory_peak',(cg/'memory.peak').read_text().strip())
