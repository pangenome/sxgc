#!/usr/bin/env python3
"""Streaming remap contract, fresh pipeline, binary MEM oracle and HTTP parity."""
import argparse
import json
import pathlib
import random
import socket
import struct
import subprocess
import tempfile
import time
import urllib.request

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--xsa', default='xsa/target/release/xsa')
p.add_argument('--log-dir', default='bit6/sxi_logs/byte-remap')
a = p.parse_args()
xsa = str(pathlib.Path(a.xsa).resolve())
logs = pathlib.Path(a.log_dir).resolve(); logs.mkdir(parents=True, exist_ok=True)

def run(cmd, ok=True):
    r = subprocess.run(list(map(str, cmd)), capture_output=True)
    assert (r.returncode == 0) == ok, (cmd, r.stderr.decode(errors='replace'))
    return r

def members(path):
    b = path.read_bytes(); n = struct.unpack_from('<I', b, 32)[0]
    return {struct.unpack_from('<I', b, 64+40*i)[0]:
            b[off:off+size] for i in range(n)
            for off, size in [struct.unpack_from('<QQ', b, 72+40*i)]}

def brute(text, pat, minimum):
    hits = set()
    for q in range(len(pat)):
        for pos in range(len(text)):
            n = 0
            while q+n < len(pat) and pos+n < len(text) and pat[q+n] == text[pos+n]: n += 1
            if n >= minimum and (q == 0 or pos == 0 or pat[q-1] != text[pos-1]):
                hits.add((q, pos, n))
    return hits

with tempfile.TemporaryDirectory(prefix='byte-remap-') as tmp:
    d = pathlib.Path(tmp)
    def build(label, data, ok=True):
        text = d/(label+'.txt'); text.write_bytes(data)
        out = d/(label+'.sxi')
        r = run([xsa, 'build', '--text', text, '-o', out, '--threads', 2,
                 '--verify-text-sample', 32, '--log-dir', logs, '--verbose'], ok)
        (logs/(label+'.build.log')).write_bytes(r.stdout+r.stderr)
        if not ok:
            assert not out.exists()
            return
        record = json.loads(r.stdout)
        work = pathlib.Path(record['work'])
        sigma = (work/'parse.remap').read_bytes()
        assert len(sigma) == len(set(sigma)) == 256 and sigma[30] == 30
        assert all(sigma[c] >= 6 for c in data)
        return out, work, sigma

    # Encounter displaced high codes AFTER low codes.
    rng = random.Random(927)
    data = bytes([0, 255, 1, 254, 2, 253, 3, 252, 4, 251, 5, 250])
    data += bytes(rng.choice([1, 2, 3, 4, 5, 65, 66, 67, 240, 255]) for _ in range(1800)) + b'\x1e'
    out, work, sigma = build('binary', data)
    assert members(out)[7] == sigma
    pats = [data[i:i+12] for i in (0, 7, 15, 77, 801)] + [b'AB\x01\x02CD', b'\xf9absent']
    fa = d/'queries.fa'
    fa.write_bytes(b''.join(b'>'+str(i).encode()+b'\n'+q+b'\n' for i,q in enumerate(pats)))
    result = run([xsa, 'mems', '--sxi', out, '--reads', fa, '--min-len', 3])
    (logs/'binary.mems.jsonl').write_bytes(result.stdout)
    got = {str(i): set() for i in range(len(pats))}
    for line in result.stdout.splitlines():
        row = json.loads(line); got[row['read']].add((row['qstart'], row['offset'], row['len']))
    for i, pat in enumerate(pats): assert got[str(i)] == brute(data, pat, 3), (i, got[str(i)], brute(data, pat, 3))
    # Exercise the same bounded text verifier used by the large-fragment gate.
    pattern_json=d/'patterns.json';pattern_json.write_text(json.dumps([
        {'name':str(i),'hex':pat.hex()} for i,pat in enumerate(pats)]))
    slices=d/'slices.json';slices.write_text(json.dumps([{'offset':0,'length':len(data)}]))
    verifier=pathlib.Path(__file__).resolve().parents[1]/'tools/pilot_verify.py'
    verify_cmd=['python3',verifier,'--text',d/'binary.txt','--patterns',pattern_json,
                '--occs',logs/'binary.mems.jsonl','--slices',slices,'--min-len',3]
    run(verify_cmd)
    missing=d/'missing.jsonl';missing.write_bytes(b'\n'.join(result.stdout.splitlines()[1:])+b'\n')
    verify_cmd[verify_cmd.index('--occs')+1]=missing
    run(verify_cmd,False)
    # Exercise the independent C++ SXI decoder and streamed dictionary path.
    provenance = json.loads(next(logs.glob('binary-'+work.name+'.jsonl')).read_text().splitlines()[0])
    slim = next(v['path'] for v in provenance['artifacts'] if v['artifact'] == 'bin/slim_dump')
    aggregate = d/'differential.agg'
    run([slim, '--slim', '--resolve-ri4', '--dict-stream', '--sxi', out,
         '--head-sa', work/'fresh.head_sa', '--parse', work/'parse', '-o', aggregate])
    assert aggregate.read_bytes() == (work/'fresh.agg').read_bytes()
    # Same encoded RI4/AGG consumed through SXI must produce identical chi bytes.
    chi = d/'differential.sA'
    run([xsa, 'chi-rspace', '--stream-agg', '--sxi', out, '--agg', work/'fresh.agg', '-o', chi])
    assert chi.read_bytes() == (work/'fresh.sA').read_bytes()
    # Identity table is journaled but omitted from the container.
    identity, _, table = build('identity', bytes(rng.choice(b'ACGT') for _ in range(5000))+b'\x1e')
    assert table == bytes(range(256)) and 7 not in members(identity)
    # Every allowed slot is usable, including the pinned separator.
    full = bytes(list(range(244))+[250,251,252,253,254,255])
    capacity, _, _ = build('capacity250', full+bytes(rng.choice(full) for _ in range(10000))+b'\x1e')
    build('overflow256', bytes(range(256))*4+b'\x1e', False)
    # JSON HTTP can represent forbidden control bytes; compare to the CLI oracle.
    with socket.socket() as s: s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    with (logs/'serve.log').open('wb') as log:
        server = subprocess.Popen([xsa, 'serve', '--sxi', str(out), '--bind', f'127.0.0.1:{port}', '-j', '2'], stdout=log, stderr=log)
        try:
            for _ in range(100):
                try:
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/stats', timeout=1): pass
                    break
                except (OSError, urllib.error.URLError): time.sleep(.1)
            query = b'AB\x01\x02CD'
            req = urllib.request.Request(f'http://127.0.0.1:{port}/batch', json.dumps({'reads': [{'pattern': query.decode()}], 'min_len': 3}).encode(), {'Content-Type':'application/json'})
            with urllib.request.urlopen(req, timeout=30) as response:
                rows = [json.loads(line) for line in response.read().splitlines()]
            assert {(r['qstart'], r['offset'], r['len']) for r in rows} == brute(data, query, 3)
        finally: server.terminate(); server.wait(timeout=10)
print('PASS remap permutation; collision displacement; binary MEM precision/recall; SXI/RI4 chi differential; identity omission; 250-code capacity; 256-code refusal; HTTP parity')
