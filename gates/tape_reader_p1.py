#!/usr/bin/env python3
"""Cold-cache native-stage comparison; retains exact commands and exit codes.

Resource flags are identical. Output plumbing necessarily differs: main uses
scatter_sort + concat, while tape_sort emits pieces directly. No timeout/signals.
"""
import json
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import re, time

root, corpus, baseline, reader = map(Path, sys.argv[1:5])
corpus_bytes = corpus.stat().st_size
assert corpus_bytes == 3_000_000_000
remap = root/'remap-256.bin'
common = ['--corpus', str(corpus), '--remap', str(remap), '--threads', '24', '--budget-bytes', str(196 << 30), '--memory-gb', '196', '--scratch-budget-bytes', str(250 << 30)]
root.mkdir()
used = set()
with corpus.open('rb') as f:
    while chunk := f.read(8 << 20):
        used.update(chunk)
if len(used) == 256:
    raise RuntimeError('all 256 bytes present: native tape consumer has no reserved END code')
table = [0]*256
for c, code in zip(sorted(used), range(1,len(used)+1)): table[c] = code
for c, code in zip(sorted(set(range(256))-used), [0]+list(range(len(used)+1,256))): table[c] = code
remap.write_bytes(bytes(table))
print(json.dumps(dict(preparation='shared order-preserving remap', corpus_bytes=corpus_bytes, used_symbols=len(used), remap_sha256=hashlib.sha256(bytes(table)).hexdigest())), flush=True)
receipts = []
for arm in ('main', 'reader'):
    run = root/arm
    for name in ('bands', 'waves', 'merged'):
        (run/name).mkdir(parents=True, exist_ok=True)
    prefix = run/'merged/frag'
    # the rchar limit is a RUNAWAY guard (100x); the 3.0 proxy is a
    # journal line, not the verdict (the operator's frozen acceptance)
    env = dict(os.environ, XSA_MEM_GUARD_MB=str(196*1024), CROSS_V2_PIECES='1', CROSS_V2_PIECE_WINDOW='1100', XSA_TAPE_WAIT_S='inf', XSA_TAPE_RCHAR_LIMIT=str(100*corpus_bytes))
    if arm == 'main':
        producer = [str(baseline/'scatter_sort'), *common, '--scratch', str(run/'waves'), '--tape-bands', str(run/'bands'), '--pending-bands', '8']
    else:
        producer = [str(reader), *common, '--out-prefix', str(prefix), '--bands', str(run/'bands')]
    finish = [str(baseline/'xsa_finish'), '--prefix', str(prefix), '--corpus', str(corpus), '--remap', str(remap), '--output', str(run/'product.sxi2'), '--threads', '24', '--poll-timeout', '0', '--wit-ram-budget', str(196 << 30), '--lcp-bands', str(run/'bands'), '--no-chi']
    commands = [producer]
    if arm == 'main':
        commands.append([str(baseline/'cross_lcp_merge'), '--binbwt', str(corpus), '--remap', str(remap), '--out-prefix', str(prefix), '--threads', '24', '--tape-bands', str(run/'bands')])
    commands.append(finish)
    with corpus.open('rb') as f:
        os.posix_fadvise(f.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
    started = time.monotonic()
    ps, logs = [], []
    for i, cmd in enumerate(commands):
        log = (run/f'stage-{i}.log').open('w'); logs.append(log)
        ps.append(subprocess.Popen(cmd, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    lastio = {}
    while any(p.poll() is None for p in ps):
        try:
            lastio = {k: int(v) for k,v in (line.split(':') for line in Path(f'/proc/{ps[0].pid}/io').read_text().splitlines())}
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            pass
        if any(p.poll() not in (None, 0) for p in ps):
            (run/'stop.requested').touch()
        time.sleep(.1)
    for log in logs: log.close()
    text = (run/'stage-0.log').read_text()
    done = re.search(r'^SS_DONE .*$', text, re.M)
    io = re.search(r'^SS_IO .*$', text, re.M)
    if io:
        lastio.update({k:int(v) for k,v in re.findall(r'(rchar|read_bytes|syscr)=(\d+)',io.group())})
    fields = dict(re.findall(r'(\w+)=([\w.]+)',done.group())) if done else {}
    rchar = lastio.get('rchar',0)
    receipt = dict(binary_sha256=hashlib.sha256(Path(producer[0]).read_bytes()).hexdigest(), binary_source=os.environ.get('TAPE_READER_COMMIT', 'unknown') if arm == 'reader' else os.environ.get('TAPE_BASELINE_COMMIT', 'unknown'), arm=arm, corpus_bytes=corpus_bytes, rows_completed=int(fields.get('n',0)), wall=time.monotonic()-started, producer_receipt=done.group() if done else None, status='COMPLETE' if done and all(p.returncode==0 for p in ps) else 'VOID', exits=[p.returncode for p in ps], io=lastio, rchar_per_corpus=rchar/corpus_bytes, flags=common, commands=commands, cold='posix_fadvise DONTNEED before arm', environment={k:env.get(k) for k in ('XSA_MEM_GUARD_MB','CROSS_V2_PIECES','CROSS_V2_PIECE_WINDOW','XSA_TAPE_WAIT_S','XSA_TAPE_RCHAR_LIMIT','XSA_BAND_WORKSPACE_BYTES','XSA_ENGINE')}, pass_requests=[x for x in text.splitlines() if x.startswith(('PASS_REQUEST ', 'PASS ', 'PASS_TOTAL '))])
    try:
        _m = re.search(r'SS_PLAN .*workspace_bytes=(\d+)', text)
        receipt['band_workspace_bytes'] = int(_m.group(1)) if _m else None
        _k = re.search(r'SS_PLAN .*bands=(\d+)', text)
        receipt['bands'] = int(_k.group(1)) if _k else None
        receipt['workspace_shape'] = 'explicit' if 'XSA_BAND_WORKSPACE_BYTES' in env else 'default-lean'
    except Exception:
        pass
    (run/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    receipts.append(receipt)
    print(json.dumps(receipt),flush=True)
(root/'receipts.json').write_text(json.dumps(receipts,indent=2)+'\n')
