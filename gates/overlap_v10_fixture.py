"""Small only. Run in a fresh constrained systemd scope (see accompanying note)."""
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

D, tools = map(lambda s: Path(s).resolve(), sys.argv[1:])
D.mkdir()  # fresh, never recursively delete evidence
G = Path(__file__).resolve().parent
cg = Path('/sys/fs/cgroup' + Path('/proc/self/cgroup').read_text().strip().split('::')[1])
assert (cg/'memory.max').read_text().strip() == str(3 << 30)
assert (cg/'memory.swap.max').read_text().strip() == '0'
assert (cg/'cpu.max').read_text().strip() == '200000 100000'
sp.run(['sha256sum', '--strict', '-c', 'fixture-tools.sha256'], cwd=tools, check=True)
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
base_env = {k:os.environ[k] for k in ('PATH','HOME','LANG') if k in os.environ}
base_env['RAYON_NUM_THREADS'] = '2'
workers = []; peak = 0


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
    until = time.monotonic() + 240
    while True:
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
    if label: print(label, "sampled whole-fixture allocated", local_peak, "scope charged", memory_peak)


def product():
    ref = D/'ref'; cand = D/'cand.sxi2'; prefix = D/'merged/frag'; bands = D/'bands'; tap = D/'tap'
    for p in (bands, tap, prefix.parent): p.mkdir()
    start('ref',[X,'build','--tape','--input',C,'--scratch',ref,'--threads','1','--memory-gb','3',
                 '--scratch-budget-gb','1','--trie-depth','8','--trie-min-support','1000','--docs-map',M]); wait('reference')
    rfile = ref/'corpus.sxi2'; remap = ref/'remap-256.bin'
    common = ['--corpus',C,'--remap',remap,'--threads','1']
    start('tap',[X,'ctx-tap','--corpus',C,'--bands',tap,'--n',str(n+1),'--out',D/'m20',
                 '--depth','8','--min-support','1000','--docs-min-support','1000','--threads','1','--doc-starts',M])
    rows = start('rows',[tools/'fixture_rows',C],env={'XSA_TEST_OVERLAP_BINNED':'1'},stdout=sp.PIPE)
    tee = start('relay',[sys.executable,G/'overlap_v10_tap.py',tap,str(n)],stdin=rows.stdout,stdout=sp.PIPE)
    rows.stdout.close()
    start('sink',[tools/'pfp_sort_r',*common,'--out-prefix',prefix,'--bands',bands,'--scratch-budget-bytes',str(128<<20)],
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
        for cross in (False,True):
            name = f'{tag}-search-{cross}'
            start(name,[X,'search',path,'--pattern','LEFTRIGHT','--jsonl','--corpus',C]+(['--cross-doc'] if cross else [])); wait()
            hits = [json.loads(l) for l in (D/f'{name}.log').read_text().splitlines() if l.startswith('{')]
            assert sorted((h['pos'],h['cross']) for h in hits) == sorted([(cut+8,0)]+([(cut-4,1)] if cross else []))


try:
    product()
finally:
    for p in workers:
        try: os.killpg(p.pid,signal.SIGKILL)
        except ProcessLookupError: pass
    for p in workers: p.wait()
    print('sampled_allocated_scratch_peak',peak,'scope_memory_peak',(cg/'memory.peak').read_text().strip())
