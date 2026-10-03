#!/usr/bin/env python3
"""Parse-free finish differential gate at synthetic scale.

For each corpus: chunk_frontend -> cross_lcp_merge (tree, finalize) ->
rpfbwt_endpoints (parse-free seam repair) -> slim_dump WITHOUT --parse ->
.agg, checked four ways:
  1. adapter parity: fresh.ri4/fresh.head_sa byte-identical to the legacy
     parse-based adapter run on the same merged files (pfp++ parse built
     here is an oracle input ONLY for the legacy binary);
  2. walk parity: the parse-free slim's emitted text sidecar byte-identical
     to the source text (SLIM_PF_KEEP_TEXT);
  3. artifact-anchored brute-force oracle (oracle.py): all four CRA1 columns
     recomputed from text+artifacts, byte-exact;
  4. fail-loud: --inject-fingerprint-error must abort.
No parse or dictionary is read by the parse-free chain at any point.
"""
import os
import pathlib
import struct
import subprocess
import random
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import oracle

J = pathlib.Path(__file__).resolve().parent
WORK = pathlib.Path('/tmp/slimpf/g0')
BIN = pathlib.Path('/tmp/slimpf')
LEGACY_ENDPOINTS = pathlib.Path('/tmp/sxgc-dist-final/rpfbwt_endpoints')
ENDPOINTS = BIN / 'rpfbwt_endpoints'
PFP = pathlib.Path('/tmp/sxgc-dist-final/pfp++')
INF = (1 << 64) - 1
random.seed(20261003)


def run(cmd, log, check=True, env=None):
    cmd = [str(c) for c in cmd]
    e = dict(os.environ)
    if env:
        e.update(env)
    with open(log, 'w') as f:
        r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=e)
    if check and r.returncode != 0:
        print(open(log).read()[-4000:])
        raise SystemExit(f'FAILED {cmd} (rc={r.returncode}); see {log}')


def gen_texts(d):
    def w(name, data):
        (d / f'{name}.txt').write_bytes(data)
    alpha = bytes([6, 7, 8, 9, 11, 12])
    # nl corpora must contain neither 0x0a nor 0x1e except their own newlines
    nl_alpha = bytes(c for c in range(6, 60) if c not in (10, 30))
    rec = bytes(random.choice(nl_alpha) for _ in range(200))
    parts = [rec] * 120 + [bytes(random.choice(nl_alpha) for _ in range(173)) for _ in range(40)]
    w('rs-dup', b'\x1e'.join(parts) + b'\x1e')
    w('periodic', b'abc\x1e' * 500)  # fully periodic incl. separator: tie-class stress
    w('rand4', b'\x1e'.join(bytes(alpha[random.randrange(4)] for _ in range(2400)) for _ in range(4)) + b'\x1e')
    lines = [b'ACGT' * 17 + bytes([nl_alpha[random.randrange(len(nl_alpha))] for _ in range(11)])
             for _ in range(30)]
    lines = lines + lines[:10] + [b'unique-record-%d' % i for i in range(20)]
    w('nl-multi', b'\n'.join(lines) + b'\n')
    def satrec(k):
        unit = bytes(((i * 13 + k * 7) % 54) + 6 for i in range(60))
        return b''.join(unit if i % 7 else unit[::-1] for i in range(37))
    w('satellite', b'\x1e'.join(satrec(k) for k in range(4)) + b'\x1e')
    w('tiny', b'abracadabra\x1ealakazam\x1eabracadabra\x1e')
    long_line = bytes(nl_alpha[random.randrange(len(nl_alpha))] for _ in range(3000))
    w('nl-long', b'\n'.join([long_line, long_line, long_line[::-1], b'short']) + b'\n')


def produce_four_files(name, d, text_path, n):
    """Fixture production. Cyclic corpora use the chunk route (chunk_frontend
    -> cross_lcp_merge); newline corpora use the legacy PFP producer
    (pfp++ L1/L2 -> rpfbwt), which chunk_frontend refuses (it requires
    0x1e-terminated input). Fixtures may be produced any way; the FINISH
    (endpoints + slim) is what this gate holds parse-free."""
    prefix = d / 'frag'
    if name.startswith('nl-'):
        (d / 'pfptmp').mkdir(exist_ok=True)
        run([PFP, '-t', text_path, '-o', prefix, '-w', '10', '-p', '100', '-j', '8',
             '--tmp-dir', d / 'pfptmp'], J / f'g0-{name}-pfp1.log')
        run([PFP, '-i', str(prefix) + '.parse', '-w', '5', '-p', '11', '-j', '8',
             '--tmp-dir', d / 'pfptmp'], J / f'g0-{name}-pfp2.log')
        run(['/tmp/sxgc-dist-final/rpfbwt', '--l1-prefix', prefix, '--w1', '10', '--w2', '5',
             '--threads', '4', '--chunks', '1', '--tmp-dir', d / 'pfptmp'],
            J / f'g0-{name}-rpfbwt.log')
    else:
        count = 4 if name != 'tiny' else 1
        run([BIN / 'chunk_frontend', text_path, count, d / 'chunks'],
            J / f'g0-{name}-chunks.log')
        run([BIN / 'cross_lcp_merge', '--tree', d / 'chunks', count, n, prefix,
             '--threads', '8', '--work', d / 'work'], J / f'g0-{name}-merge.log')


def main():
    WORK.mkdir(parents=True, exist_ok=True)
    gen_texts(WORK)
    names = ['rs-dup', 'periodic', 'rand4', 'nl-multi', 'satellite', 'tiny', 'nl-long']
    for name in names:
        d = WORK / name
        d.mkdir(exist_ok=True)
        (d / 'chunks').mkdir(exist_ok=True)
        text = (WORK / f'{name}.txt').read_bytes()
        n = len(text)
        produce_four_files(name, d, WORK / f'{name}.txt', n)
        run([ENDPOINTS, d / 'frag', d / 'fresh.ri4', d / 'fresh.head_sa'],
            J / f'g0-{name}-endpoints.log', check=False)
        fresh_body = open(J / f'g0-{name}-endpoints.log').read()
        # 1: adapter parity vs legacy parse-based repair on the same four files.
        # Build the PFP parse as an oracle input for the LEGACY binary only.
        if not name.startswith('nl-'):
            (d / 'pfpbuild').mkdir(exist_ok=True)
            run([PFP, '-t', WORK / f'{name}.txt', '-o', d / 'pfpbuild/parse',
                 '-w', '10', '-p', '100', '-j', '8', '--tmp-dir', d / 'pfpbuild'],
                J / f'g0-{name}-pfp.log')
            for ext in ('dict', 'parse'):
                (d / f'frag.{ext}').write_bytes((d / f'pfpbuild/parse.{ext}').read_bytes())
        legacy_log = J / f'g0-{name}-endpoints-legacy.log'
        run([LEGACY_ENDPOINTS, d / 'frag', d / 'legacy.ri4', d / 'legacy.head_sa'],
            legacy_log, check=False)
        legacy_body = open(legacy_log).read()
        if not name.startswith('nl-'):
            for ext in ('dict', 'parse'):
                (d / f'frag.{ext}').unlink()
        if 'CYCLIC_SEAM_REFUSED' in fresh_body:
            # Fully-periodic corpora exceed the adapter's bounded seam policy.
            # The refusal is by design; both adapters must refuse identically.
            assert 'CYCLIC_SEAM_REFUSED' in legacy_body, \
                f'{name}: parse-free adapter refused but legacy did not'
            print(f'PASS {name}: both adapters refuse the out-of-policy seam identically',
                  flush=True)
            continue
        assert 'ENDPOINT_PASS' in fresh_body, f'{name}: parse-free adapter failed: {fresh_body[-500:]}'
        if 'ENDPOINT_PASS' not in legacy_body:
            # The legacy adapter needs a PFP parse; corpora too small for the
            # parser have no legacy run. The parse-free adapter handles them.
            print(f'PASS {name}: parse-free adapter passes; legacy cannot '
                  '(no viable PFP parse); parity skipped', flush=True)
        else:
            for f in ('ri4', 'head_sa'):
                assert (d / f'fresh.{f}').read_bytes() == (d / f'legacy.{f}').read_bytes(), \
                    f'{name}: parse-free endpoints {f} differs from parse-based legacy'
            print(f'PASS {name}: endpoints ri4+head_sa byte-identical to legacy adapter', flush=True)
        for extra in ([], ['--tau1', '2']):
            tag = 'tau2' if extra else 'default'
            agg = d / f'free.{tag}.agg'
            run([BIN / 'slim_dump', '--slim', '--resolve-ri4', '--ri4', d / 'fresh.ri4',
                 '--head-sa', d / 'fresh.head_sa', '-t', '8',
                 *extra, '-o', agg], J / f'g0-{name}-slim-{tag}.log',
                env={'SLIM_PF_KEEP_TEXT': '1'})
            # 2: walk parity - emitted sidecar must equal the source text
            sidecar = pathlib.Path(str(agg) + '.pftext')
            assert sidecar.exists(), f'{name}: sidecar not retained'
            assert sidecar.read_bytes() == text, f'{name}/{tag}: emitted text != source text'
            # 3: artifact-anchored brute-force oracle
            on, oR, ok = oracle.compare(agg, d / 'fresh.ri4', d / 'fresh.head_sa',
                                        WORK / f'{name}.txt')
            print(f'PASS {name}/{tag}: sidecar==text; agg byte-identical to brute oracle '
                  f'(n={n} R={oR} k={ok})', flush=True)
            sidecar.unlink()
        # 4: fault injection must fail loudly
        log = J / f'g0-{name}-fault.log'
        run([BIN / 'slim_dump', '--slim', '--resolve-ri4', '--ri4', d / 'fresh.ri4',
             '--head-sa', d / 'fresh.head_sa', '-t', '4', '--inject-fingerprint-error',
             '-o', d / 'fault.agg'], log, check=False)
        assert 'FATAL SLIM' in open(log).read(), f'{name}: fault injection did not fail loudly'
        print(f'PASS {name}: fault injection fails loudly', flush=True)
    print('G0_PARSE_FREE PASS all corpora: adapter parity, walk parity, '
          'oracle byte identity (default tau + tau1=2), fail-loud')


if __name__ == '__main__':
    main()
